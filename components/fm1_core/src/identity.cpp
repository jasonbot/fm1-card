#include "fm1/identity.hpp"

#include <algorithm>
#include <cctype>

namespace fm1::identity {
namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

std::string_view model_of(std::string_view identity) {
  identity = strip_ota(identity);
  const auto at = identity.find('_');
  return at == std::string_view::npos ? identity : identity.substr(0, at);
}

std::string_view version_of(std::string_view identity) {
  identity = strip_ota(identity);
  const auto at = identity.find('_');
  return at == std::string_view::npos ? std::string_view{} : identity.substr(at + 1);
}

bool is_smk37_model(std::string_view model) {
  model = trim(model);
  const auto text = lower(model);
  return text.starts_with("smk-37") || text == "mke-p37" || text == "starrykey-37 play";
}

bool same_model(std::string_view device_model, std::string_view package_model) {
  device_model = trim(device_model);
  package_model = trim(package_model);
  if (lower(device_model) == lower(package_model)) {
    return true;
  }
  return lower(device_model) == "starrykey-37 play" && lower(package_model) == "smk-37 pro";
}

bool package_fits(std::string_view current, std::string_view product) {
  current = strip_ota(current);
  const auto device_model = model_of(current);
  const auto device_version = version_of(current);
  const auto package_model = model_of(product);
  if (same_model(device_model, package_model)) {
    return true;
  }
  return is_smk37_model(device_model) && is_smk37_model(package_model) &&
         !device_version.starts_with('0');
}

std::string_view strip_ota(std::string_view text) {
  if (text.starts_with("ota-")) {
    return text.substr(4);
  }
  if (text.starts_with("OTA-")) {
    return text.substr(4);
  }
  return text;
}

std::string_view device_type(std::string_view identity) {
  const auto model = model_of(identity);
  if (lower(model) == "fm-1") {
    return "FM-1";
  }
  if (lower(model) == "smk-37 pro" || lower(model) == "starrykey-37 play") {
    return "SMK-37 Pro";
  }
  if (lower(model) == "smk-37 elite") {
    return "SMK-37 Elite";
  }
  if (lower(model) == "mke-p37") {
    return "MKE-P37";
  }
  return {};
}

bool same_identity(std::string_view reported, std::string_view product) {
  const auto reported_model = model_of(reported);
  const auto reported_version = version_of(reported);
  const auto product_model = model_of(product);
  const auto product_version = version_of(product);
  // Some builds report only a prefix of their version. A missing version is
  // not a prefix.
  const bool version_ok =
      reported_version == product_version ||
      (!reported_version.empty() && product_version.starts_with(reported_version));
  return same_model(reported_model, product_model) && version_ok;
}

}  // namespace fm1::identity
