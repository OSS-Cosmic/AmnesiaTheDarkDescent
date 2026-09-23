// Build-time DXIL blob-to-header generator.
//
// This tool intentionally has no engine/runtime dependencies.  It turns the
// DXIL files produced by slangc into deterministic C++ include files so the
// D3D12 build does not need a runtime compiler or checked-in binary blobs.

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Input {
  std::filesystem::path path;
  std::string name;
};

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " --output <header> [--namespace <name>]"
               " --input <dxil> [--name <identifier>] ...\n";
}

bool identifier(std::string_view value) {
  if (value.empty() || (std::isdigit(static_cast<unsigned char>(value.front())) != 0))
    return false;
  for (const char c : value) {
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') return false;
  }
  return true;
}

std::string make_identifier(const std::filesystem::path& path) {
  std::string result;
  for (const char c : path.stem().string()) {
    result += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';
  }
  if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front())) != 0)
    result.insert(result.begin(), '_');
  return result + "_dxil";
}

std::string namespace_open(std::string_view ns) {
  std::string result;
  std::size_t begin = 0;
  while (begin < ns.size()) {
    const std::size_t end = ns.find("::", begin);
    const std::string_view component = ns.substr(begin, end == std::string_view::npos
                                                         ? ns.size() - begin
                                                         : end - begin);
    if (!identifier(component)) return {};
    result += "namespace ";
    result += component;
    result += " { ";
    if (end == std::string_view::npos) break;
    begin = end + 2;
  }
  return result;
}

std::size_t namespace_depth(std::string_view ns) {
  if (ns.empty()) return 0;
  std::size_t depth = 1;
  for (std::size_t at = 0; (at = ns.find("::", at)) != std::string_view::npos; at += 2)
    ++depth;
  return depth;
}

int generate(const std::filesystem::path& output, std::string_view ns,
             const std::vector<Input>& inputs) {
  std::error_code error;
  if (!output.parent_path().empty())
    std::filesystem::create_directories(output.parent_path(), error);
  if (error) {
    std::cerr << "ri_shader_embed: cannot create output directory: " << error.message() << '\n';
    return 1;
  }

  std::ofstream header(output, std::ios::binary | std::ios::trunc);
  if (!header) {
    std::cerr << "ri_shader_embed: cannot write " << output << '\n';
    return 1;
  }
  header << "#pragma once\n#include <cstddef>\n#include <cstdint>\n\n";
  header << namespace_open(ns) << '\n';
  header << "struct ShaderBlob { const std::uint8_t* data; std::size_t size; };\n\n";

  for (const Input& input : inputs) {
    std::ifstream file(input.path, std::ios::binary);
    if (!file) {
      std::cerr << "ri_shader_embed: cannot read " << input.path << '\n';
      return 1;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() < 4 || bytes[0] != 'D' || bytes[1] != 'X' || bytes[2] != 'B' ||
        bytes[3] != 'C') {
      std::cerr << "ri_shader_embed: " << input.path << " is not a DXBC/DXIL container\n";
      return 1;
    }

    header << "inline constexpr std::uint8_t " << input.name << "[] = {\n  ";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if (i != 0) header << ((i % 12 == 0) ? "\n  " : " ");
      header << "0x" << std::hex << static_cast<unsigned>(bytes[i]) << std::dec;
      if (i + 1 != bytes.size()) header << ',';
    }
    header << "\n};\n";
    header << "inline constexpr ShaderBlob " << input.name
           << "_blob{" << input.name << ", sizeof(" << input.name << ")};\n\n";
  }

  for (std::size_t i = 0; i < namespace_depth(ns); ++i) header << "}\n";
  if (!header) {
    std::cerr << "ri_shader_embed: failed while writing " << output << '\n';
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path output;
  std::string ns = "hpl::ri_embedded";
  std::vector<Input> inputs;
  std::string pending_name;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (option == "--output" && i + 1 < argc) output = argv[++i];
    else if (option == "--namespace" && i + 1 < argc) ns = argv[++i];
    else if (option == "--name" && i + 1 < argc) pending_name = argv[++i];
    else if (option == "--input" && i + 1 < argc) {
      Input input{argv[++i], pending_name.empty() ? make_identifier(argv[i]) : pending_name};
      pending_name.clear();
      if (!identifier(input.name)) {
        std::cerr << "ri_shader_embed: invalid C++ identifier: " << input.name << '\n';
        return 2;
      }
      inputs.push_back(std::move(input));
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  if (output.empty() || inputs.empty() || !pending_name.empty() || namespace_open(ns).empty()) {
    usage(argv[0]);
    return 2;
  }
  return generate(output, ns, inputs);
}
