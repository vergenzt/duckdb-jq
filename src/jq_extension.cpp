#define DUCKDB_EXTENSION_MAIN

#include "jq_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

inline void JqScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto jq_scalar_function =
	    ScalarFunction("jq", {LogicalType::VARCHAR}, LogicalType::VARCHAR, JqScalarFun);

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
