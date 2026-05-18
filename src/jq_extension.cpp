#define DUCKDB_EXTENSION_MAIN

#include "jq_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
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
	JqLocalState() : jq_state(nullptr) {
	}
	~JqLocalState() override {
		if (jq_state) {
			jq_teardown(&jq_state);
		}
	}
	jq_state *jq_state;
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
	local->jq_state = jq_init();
	if (!local->jq_state) {
		throw InvalidInputException("jq: failed to allocate jq state");
	}
	if (!jq_compile(local->jq_state, info.filter.c_str())) {
		throw InvalidInputException("jq: failed to compile filter '%s'", info.filter);
	}
	return std::move(local);
}

void JqScalarFun(DataChunk &args, ExpressionState &state, Vector &result_vec) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<JqLocalState>();
	auto jq_state = lstate.jq_state;
	auto inputs_vec = args.data[0];

	auto input_handler = [&](string_t input, ValidityMask &mask, idx_t idx) -> string_t {
		// TODO: add a setting for whether to coerce input/output JSON nulls to/from SQL nulls?

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

		jq_start(jq_state, input_jv, 0); // (input_jv consumed)
		jv result_jv = jq_next(jq_state);

		// no result -> SQL null
		if (!jv_is_valid(result_jv)) {
			jv_free(result_jv);
			mask.SetInvalid(idx);
			return string_t();
		}

		// extra result -> error
		// (TODO: add a `jq_multi` function permitting multiple results, and "unnesting" them?)
		jv extra_jv = jq_next(jq_state);
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
