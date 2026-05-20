#include "reality/http.hpp"
#include "reality/json.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using reality::json::Value;

namespace {

std::string env_or(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::string(value) : fallback;
}

std::string arg_value(int argc, char** argv, const std::string& flag, const std::string& fallback = "") {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == flag) return argv[i + 1];
  }
  return fallback;
}

bool has_flag(int argc, char** argv, const std::string& flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == flag) return true;
  }
  return false;
}

std::string positional(int argc, char** argv, int index) {
  int seen = 0;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--", 0) == 0) {
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) ++i;
      continue;
    }
    if (seen++ == index) return arg;
  }
  return "";
}

Value::Array parse_values(const std::string& csv) {
  Value::Array values;
  std::stringstream stream(csv);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (item.empty()) continue;
    values.emplace_back(std::stod(item));
  }
  return values;
}

std::string pe_url(int argc, char** argv) {
  std::string url = arg_value(argc, argv, "--pe-url", env_or("PE_URL", "http://localhost:3300"));
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

void usage() {
  std::cerr
    << "Usage:\n"
    << "  reality_engine_cli pe integrations-status [--pe-url URL]\n"
    << "  reality_engine_cli pe dispatch-ledger [--pe-url URL]\n"
    << "  reality_engine_cli pe dispatch-read ID [--pe-url URL]\n"
    << "  reality_engine_cli pe dispatch-update ID [--status STATUS] [--adapter NAME] [--provider NAME]\n"
    << "      [--external-run-id ID] [--increment-attempts] [--error TEXT] [--json JSON] [--pe-url URL]\n"
    << "  reality_engine_cli pe ollama-status [--pe-url URL]\n"
    << "  reality_engine_cli pe ollama-dispatch ID [--model NAME] [--source-mapping-id ID]\n"
    << "      [--prompt TEXT] [--no-commit] [--trigger-push] [--json JSON] [--pe-url URL]\n"
    << "  reality_engine_cli pe openai-status [--pe-url URL]\n"
    << "  reality_engine_cli pe openai-dispatch ID [--model NAME] [--source-mapping-id ID]\n"
    << "      [--prompt TEXT] [--instructions TEXT] [--no-commit] [--trigger-push] [--json JSON] [--pe-url URL]\n"
    << "  reality_engine_cli pe healthkit-status [--pe-url URL]\n"
    << "  reality_engine_cli pe healthkit-ingest --sample-type TYPE --values CSV [--source-mapping-id ID]\n"
    << "      [--bridge-id ID] [--unit UNIT] [--trigger-push] [--json JSON] [--pe-url URL]\n"
    << "  reality_engine_cli pe carekit-status [--pe-url URL]\n"
    << "  reality_engine_cli pe carekit-ingest --sample-type TYPE --values CSV [--source-mapping-id ID]\n"
    << "      [--bridge-id ID] [--task-id ID] [--care-plan-id ID] [--trigger-push] [--json JSON] [--pe-url URL]\n"
    << "  reality_engine_cli pe completion --values CSV [--provider NAME] [--agent NAME]\n"
    << "      [--source-mapping-id ID] [--correlation-id ID] [--envelope-id ID] [--trigger-push] [--json JSON]\n";
}

int run_pe(int argc, char** argv) {
  std::string command = positional(argc, argv, 1);
  std::string base = pe_url(argc, argv);

  if (command == "integrations-status") {
    std::cout << reality::http::get(base + "/api/integrations/status") << "\n";
    return 0;
  }
  if (command == "dispatch-ledger") {
    std::cout << reality::http::get(base + "/api/dispatch/ledger") << "\n";
    return 0;
  }
  if (command == "dispatch-read") {
    std::string id = positional(argc, argv, 2);
    if (id.empty()) throw std::invalid_argument("dispatch-read requires ID");
    std::cout << reality::http::get(base + "/api/dispatch/records/" + id) << "\n";
    return 0;
  }
  if (command == "dispatch-update") {
    std::string id = positional(argc, argv, 2);
    if (id.empty()) throw std::invalid_argument("dispatch-update requires ID");
    std::string raw = arg_value(argc, argv, "--json");
    if (raw.empty()) {
      Value::Object body;
      if (has_flag(argc, argv, "--increment-attempts")) body["incrementAttempts"] = true;
      std::string status = arg_value(argc, argv, "--status");
      std::string adapter = arg_value(argc, argv, "--adapter");
      std::string provider = arg_value(argc, argv, "--provider");
      std::string externalRunId = arg_value(argc, argv, "--external-run-id");
      std::string error = arg_value(argc, argv, "--error");
      if (!status.empty()) body["status"] = status;
      if (!adapter.empty()) body["adapter"] = adapter;
      if (!provider.empty()) body["provider"] = provider;
      if (!externalRunId.empty()) body["externalRunId"] = externalRunId;
      if (!error.empty()) body["error"] = error;
      raw = reality::json::stringify(Value(body));
    }
    std::cout << reality::http::request_json("PATCH", base + "/api/dispatch/records/" + id, raw) << "\n";
    return 0;
  }
  if (command == "ollama-status") {
    std::cout << reality::http::get(base + "/api/integrations/ollama/status") << "\n";
    return 0;
  }
  if (command == "ollama-dispatch") {
    std::string id = positional(argc, argv, 2);
    if (id.empty()) throw std::invalid_argument("ollama-dispatch requires ID");
    std::string raw = arg_value(argc, argv, "--json");
    if (raw.empty()) {
      Value::Object body;
      body["dispatchId"] = id;
      std::string model = arg_value(argc, argv, "--model");
      std::string mappingId = arg_value(argc, argv, "--source-mapping-id", arg_value(argc, argv, "--mapping-id"));
      std::string prompt = arg_value(argc, argv, "--prompt");
      if (!model.empty()) body["model"] = model;
      if (!mappingId.empty()) body["sourceMappingId"] = mappingId;
      if (!prompt.empty()) body["prompt"] = prompt;
      if (has_flag(argc, argv, "--no-commit")) body["commitCompletion"] = false;
      if (has_flag(argc, argv, "--trigger-push")) body["triggerPush"] = true;
      raw = reality::json::stringify(Value(body));
    }
    std::cout << reality::http::request_json("POST", base + "/api/integrations/ollama/dispatch", raw) << "\n";
    return 0;
  }
  if (command == "openai-status") {
    std::cout << reality::http::get(base + "/api/integrations/openai/status") << "\n";
    return 0;
  }
  if (command == "openai-dispatch") {
    std::string id = positional(argc, argv, 2);
    if (id.empty()) throw std::invalid_argument("openai-dispatch requires ID");
    std::string raw = arg_value(argc, argv, "--json");
    if (raw.empty()) {
      Value::Object body;
      body["dispatchId"] = id;
      std::string model = arg_value(argc, argv, "--model");
      std::string mappingId = arg_value(argc, argv, "--source-mapping-id", arg_value(argc, argv, "--mapping-id"));
      std::string prompt = arg_value(argc, argv, "--prompt");
      std::string instructions = arg_value(argc, argv, "--instructions");
      if (!model.empty()) body["model"] = model;
      if (!mappingId.empty()) body["sourceMappingId"] = mappingId;
      if (!prompt.empty()) body["prompt"] = prompt;
      if (!instructions.empty()) body["instructions"] = instructions;
      if (has_flag(argc, argv, "--no-commit")) body["commitCompletion"] = false;
      if (has_flag(argc, argv, "--trigger-push")) body["triggerPush"] = true;
      raw = reality::json::stringify(Value(body));
    }
    std::cout << reality::http::request_json("POST", base + "/api/integrations/openai/dispatch", raw) << "\n";
    return 0;
  }
  if (command == "healthkit-status") {
    std::cout << reality::http::get(base + "/api/integrations/healthkit/status") << "\n";
    return 0;
  }
  if (command == "healthkit-ingest") {
    std::string raw = arg_value(argc, argv, "--json");
    if (raw.empty()) {
      std::string values = arg_value(argc, argv, "--values");
      std::string sampleType = arg_value(argc, argv, "--sample-type", arg_value(argc, argv, "--type"));
      if (values.empty() || sampleType.empty()) throw std::invalid_argument("healthkit-ingest requires --sample-type TYPE and --values CSV, or --json JSON");
      Value::Object body;
      body["sampleType"] = sampleType;
      body["values"] = parse_values(values);
      std::string bridgeId = arg_value(argc, argv, "--bridge-id");
      std::string mappingId = arg_value(argc, argv, "--source-mapping-id", arg_value(argc, argv, "--mapping-id"));
      std::string unit = arg_value(argc, argv, "--unit");
      if (!bridgeId.empty()) body["bridgeId"] = bridgeId;
      if (!mappingId.empty()) body["sourceMappingId"] = mappingId;
      if (!unit.empty()) body["unit"] = unit;
      if (has_flag(argc, argv, "--trigger-push")) body["triggerPush"] = true;
      raw = reality::json::stringify(Value(body));
    }
    std::cout << reality::http::request_json("POST", base + "/api/integrations/healthkit/ingest", raw) << "\n";
    return 0;
  }
  if (command == "carekit-status") {
    std::cout << reality::http::get(base + "/api/integrations/carekit/status") << "\n";
    return 0;
  }
  if (command == "carekit-ingest") {
    std::string raw = arg_value(argc, argv, "--json");
    if (raw.empty()) {
      std::string values = arg_value(argc, argv, "--values");
      std::string sampleType = arg_value(argc, argv, "--sample-type", arg_value(argc, argv, "--type"));
      if (values.empty() || sampleType.empty()) throw std::invalid_argument("carekit-ingest requires --sample-type TYPE and --values CSV, or --json JSON");
      Value::Object body;
      body["sampleType"] = sampleType;
      body["values"] = parse_values(values);
      std::string bridgeId = arg_value(argc, argv, "--bridge-id");
      std::string mappingId = arg_value(argc, argv, "--source-mapping-id", arg_value(argc, argv, "--mapping-id"));
      std::string taskId = arg_value(argc, argv, "--task-id");
      std::string carePlanId = arg_value(argc, argv, "--care-plan-id");
      if (!bridgeId.empty()) body["bridgeId"] = bridgeId;
      if (!mappingId.empty()) body["sourceMappingId"] = mappingId;
      if (!taskId.empty()) body["taskId"] = taskId;
      if (!carePlanId.empty()) body["carePlanId"] = carePlanId;
      if (has_flag(argc, argv, "--trigger-push")) body["triggerPush"] = true;
      raw = reality::json::stringify(Value(body));
    }
    std::cout << reality::http::request_json("POST", base + "/api/integrations/carekit/ingest", raw) << "\n";
    return 0;
  }
  if (command == "completion") {
    std::string raw = arg_value(argc, argv, "--json");
    if (raw.empty()) {
      std::string values = arg_value(argc, argv, "--values");
      if (values.empty()) throw std::invalid_argument("completion requires --values CSV or --json JSON");
      Value::Object body;
      body["provider"] = arg_value(argc, argv, "--provider", "cli");
      body["agent"] = arg_value(argc, argv, "--agent", "cli");
      std::string mappingId = arg_value(argc, argv, "--source-mapping-id", arg_value(argc, argv, "--mapping-id"));
      std::string correlationId = arg_value(argc, argv, "--correlation-id");
      std::string envelopeId = arg_value(argc, argv, "--envelope-id");
      if (!mappingId.empty()) body["sourceMappingId"] = mappingId;
      if (!correlationId.empty()) body["correlationId"] = correlationId;
      if (!envelopeId.empty()) body["envelopeId"] = envelopeId;
      body["values"] = parse_values(values);
      body["triggerPush"] = has_flag(argc, argv, "--trigger-push");
      raw = reality::json::stringify(Value(body));
    }
    std::cout << reality::http::request_json("POST", base + "/api/integrations/completions", raw) << "\n";
    return 0;
  }

  usage();
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 3 || std::string(argv[1]) != "pe") {
      usage();
      return 2;
    }
    return run_pe(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "reality_engine_cli: " << e.what() << "\n";
    return 1;
  }
}
