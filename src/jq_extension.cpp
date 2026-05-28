#define DUCKDB_EXTENSION_MAIN

#include "jq_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

extern "C" {
#include <jq.h>
#include <jv.h>
}

namespace duckdb {

struct JqBindData : public FunctionData {
	explicit JqBindData(string filter_p) : filter(std::move(filter_p)) {
	}
	string filter;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JqBindData>(filter);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JqBindData>();
		return filter == other.filter;
	}
};

struct JqLocalState : public FunctionLocalState {
	JqLocalState() : jq(nullptr) {
	}
	~JqLocalState() override {
		if (jq) {
			jq_teardown(&jq);
		}
	}
	jq_state *jq;
};

static void JqErrorCb(void *data, jv msg) {
	auto &err = *static_cast<string *>(data);
	if (jv_get_kind(msg) != JV_KIND_STRING) {
		msg = jq_format_error(msg);
	}
	err += jv_string_value(msg);
	jv_free(msg);
}

unique_ptr<FunctionData> JqScalarBind(ClientContext &context, ScalarFunction &bound_function,
                                      vector<unique_ptr<Expression>> &arguments) {
	auto &filter_arg = arguments[1];
	if (filter_arg->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (!filter_arg->IsFoldable()) {
		throw InvalidInputException(*filter_arg, "jq filter must be a constant expression");
	}
	Value filter_value = ExpressionExecutor::EvaluateScalar(context, *filter_arg);
	if (filter_value.IsNull()) {
		throw InvalidInputException(*filter_arg, "jq filter must not be NULL");
	}
	string filter = filter_value.GetValue<string>();

	jq_state *probe = jq_init();
	if (!probe) {
		throw InvalidInputException("jq: failed to allocate jq state");
	}
	string err;
	jq_set_error_cb(probe, JqErrorCb, &err);
	int ok = jq_compile(probe, filter.c_str());
	jq_teardown(&probe);
	if (!ok) {
		throw InvalidInputException(*filter_arg, "jq: failed to compile filter '%s': %s", filter, err);
	}

	return make_uniq<JqBindData>(std::move(filter));
}

static unique_ptr<FunctionLocalState> JqInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                       FunctionData *bind_data) {
	auto &info = bind_data->Cast<JqBindData>();
	auto local = make_uniq<JqLocalState>();
	local->jq = jq_init();
	if (!local->jq) {
		throw InvalidInputException("jq: failed to allocate jq state");
	}
	if (!jq_compile(local->jq, info.filter.c_str())) {
		throw InvalidInputException("jq: failed to compile filter '%s'", info.filter);
	}
	return std::move(local);
}

// ==== jq_unnest table function ====

struct JqUnnestBindData : public TableFunctionData {
	string json_input;
	string filter;
	bool json_is_null;

	JqUnnestBindData(string json_input_p, string filter_p, bool json_is_null_p)
	    : json_input(std::move(json_input_p)), filter(std::move(filter_p)), json_is_null(json_is_null_p) {
	}
};

struct JqUnnestGlobalState : public GlobalTableFunctionState {
	jq_state *jq = nullptr;
	bool done = false;

	~JqUnnestGlobalState() override {
		if (jq) {
			jq_teardown(&jq);
		}
	}
};

static unique_ptr<FunctionData> JqUnnestBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::JSON());
	names.push_back("value");

	bool json_is_null = input.inputs[0].IsNull();
	string json_input = json_is_null ? "" : input.inputs[0].GetValue<string>();

	if (input.inputs[1].IsNull()) {
		throw InvalidInputException("jq filter must not be NULL");
	}
	string filter = input.inputs[1].GetValue<string>();

	jq_state *probe = jq_init();
	if (!probe) {
		throw InvalidInputException("jq: failed to allocate jq state");
	}
	string err;
	jq_set_error_cb(probe, JqErrorCb, &err);
	int ok = jq_compile(probe, filter.c_str());
	jq_teardown(&probe);
	if (!ok) {
		throw InvalidInputException("jq: failed to compile filter '%s': %s", filter, err);
	}

	return make_uniq<JqUnnestBindData>(std::move(json_input), std::move(filter), json_is_null);
}

static unique_ptr<GlobalTableFunctionState> JqUnnestInitGlobal(ClientContext &context,
                                                                TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<JqUnnestBindData>();
	auto state = make_uniq<JqUnnestGlobalState>();

	if (bind_data.json_is_null) {
		state->done = true;
		return state;
	}

	state->jq = jq_init();
	if (!state->jq) {
		throw InvalidInputException("jq: failed to allocate jq state");
	}
	if (!jq_compile(state->jq, bind_data.filter.c_str())) {
		throw InvalidInputException("jq: failed to compile filter '%s'", bind_data.filter);
	}

	jv input_jv = jv_parse_sized(bind_data.json_input.c_str(), UnsafeNumericCast<int>(bind_data.json_input.size()));
	if (!jv_is_valid(input_jv)) {
		jv msg = jv_invalid_get_msg(input_jv);
		string err = jv_get_kind(msg) == JV_KIND_STRING ? jv_string_value(msg) : "invalid JSON";
		jv_free(msg);
		throw InvalidInputException("jq: failed to parse input JSON: %s", err);
	}

	jq_start(state->jq, input_jv, 0); // (input_jv consumed)
	return state;
}

static void JqUnnestScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<JqUnnestGlobalState>();

	if (state.done) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = 0;
	auto result_data = FlatVector::GetData<string_t>(output.data[0]);
	while (count < STANDARD_VECTOR_SIZE) {
		jv result_jv = jq_next(state.jq);
		if (!jv_is_valid(result_jv)) {
			jv_free(result_jv);
			state.done = true;
			break;
		}
		jv result_str = jv_dump_string(result_jv, 0); // (result_jv consumed)
		idx_t len = UnsafeNumericCast<idx_t>(jv_string_length_bytes(jv_copy(result_str)));
		result_data[count++] = StringVector::AddString(output.data[0], jv_string_value(result_str), len);
		jv_free(result_str);
	}
	output.SetCardinality(count);
}

void JqScalarFun(DataChunk &args, ExpressionState &state, Vector &result_vec) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<JqLocalState>();
	auto jq = lstate.jq;
	auto inputs_vec = args.data[0];

	auto input_handler = [&](string_t input, ValidityMask &mask, idx_t idx) -> string_t {
		// TODO: can we get rid of the UnsafeNumericCast?
		// > main problem is that jv_parse_sized takes an i32 but input.GetSize() is a u64.
		// > what happens if input is longer than 2^32? (4 GiB)
		// > https://duckdb.org/docs/current/operations_manual/limits says the "default" string size limit is 4GB
		// > ... is that configurable?
		jv input_jv = jv_parse_sized(input.GetData(), UnsafeNumericCast<int>(input.GetSize()));
		if (!jv_is_valid(input_jv)) {
			jv msg = jv_invalid_get_msg(input_jv);
			string err = jv_get_kind(msg) == JV_KIND_STRING ? jv_string_value(msg) : "invalid JSON";
			jv_free(msg);
			throw InvalidInputException("jq: failed to parse input JSON: %s", err);
		}

		jq_start(jq, input_jv, 0); // (input_jv consumed)
		jv result_jv = jq_next(jq);

		// no result -> SQL null
		if (!jv_is_valid(result_jv)) {
			jv_free(result_jv);
			mask.SetInvalid(idx);
			return string_t();
		}

		// extra result -> error (use jq_unnest for filters that produce multiple results)
		jv extra_jv = jq_next(jq);
		if (jv_is_valid(extra_jv)) {
			jv_free(result_jv);
			jv_free(extra_jv);
			throw OutOfRangeException("jq: scalar function expected 1 result but filter produced more than 1");
		}
		jv_free(extra_jv);

		// result is good -> dump it to string
		jv result_jv_str = jv_dump_string(result_jv, 0);
		idx_t result_str_len = UnsafeNumericCast<idx_t>(jv_string_length_bytes(jv_copy(result_jv_str)));
		const char *result_str = jv_string_value(result_jv_str);
		string_t out = StringVector::AddString(result_vec, result_str, result_str_len);
		jv_free(result_jv_str);

		return out;
	};

	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(inputs_vec, result_vec, args.size(), input_handler);
}

static void LoadInternal(ExtensionLoader &loader) {
	auto jq_scalar_function = ScalarFunction("jq", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::JSON(),
	                                         JqScalarFun, JqScalarBind, nullptr, nullptr, JqInitLocalState);
	loader.RegisterFunction(jq_scalar_function);

	TableFunction jq_unnest_function("jq_unnest", {LogicalType::VARCHAR, LogicalType::VARCHAR}, JqUnnestScan,
	                                 JqUnnestBind, JqUnnestInitGlobal);
	loader.RegisterFunction(jq_unnest_function);
}

void JqExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string JqExtension::Name() {
	return "jq";
}

std::string JqExtension::Version() const {
#ifdef EXT_VERSION_JQ
	return EXT_VERSION_JQ;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(jq, loader) {
	duckdb::LoadInternal(loader);
}
}
