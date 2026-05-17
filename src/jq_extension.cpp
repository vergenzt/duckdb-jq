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

void JqScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<JqLocalState>();
	auto jq = lstate.jq;

	UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
	    args.data[0], result, args.size(), [&](string_t input, ValidityMask &mask, idx_t idx) -> string_t {
		    jv parsed = jv_parse_sized(input.GetData(), UnsafeNumericCast<int>(input.GetSize()));
		    if (!jv_is_valid(parsed)) {
			    jv msg = jv_invalid_get_msg(parsed);
			    string err = jv_get_kind(msg) == JV_KIND_STRING ? jv_string_value(msg) : "invalid JSON";
			    jv_free(msg);
			    throw InvalidInputException("jq: failed to parse input JSON: %s", err);
		    }

		    jq_start(jq, parsed, 0);

		    vector<jv> results;
		    for (jv r = jq_next(jq); jv_is_valid(r); r = jq_next(jq)) {
			    results.push_back(r);
		    }

		    if (results.empty()) {
			    mask.SetInvalid(idx);
			    return string_t();
		    }

		    jv final_value;
		    if (results.size() == 1) {
			    final_value = results[0];
		    } else {
			    final_value = jv_array();
			    for (auto &r : results) {
				    final_value = jv_array_append(final_value, r);
			    }
		    }

		    jv dumped = jv_dump_string(final_value, 0);
		    int len = jv_string_length_bytes(jv_copy(dumped));
		    string_t out = StringVector::AddString(result, jv_string_value(dumped), UnsafeNumericCast<idx_t>(len));
		    jv_free(dumped);
		    return out;
	    });
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
