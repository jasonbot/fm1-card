#pragma once

// M-VAVE FM-1 / SMK-37 identity rules, shared by the UI and the flash flow.
// Ported from fm1-grid's hardware identity module; the rules are the vendor's.

#include <string>
#include <string_view>

namespace fm1::identity {

bool is_smk37_model(std::string_view model);
bool same_model(std::string_view device_model, std::string_view package_model);

// Whether the running firmware will accept the package. Stock apps check their
// own model (a Starrykey counts as an SMK-37 Pro); custom SMK firmware (7xx/9xx)
// accepts any SMK model. FM-1 and SMK never cross.
bool package_fits(std::string_view current, std::string_view product);

// Identity without the update-loader "ota-" prefix.
std::string_view strip_ota(std::string_view text);

// Friendly device family, or empty.
std::string_view device_type(std::string_view identity);

// The identity a device reports after a flash: same model family at the same
// version, allowing a reported version prefix.
bool same_identity(std::string_view reported, std::string_view product);

// "FM-1_916" -> "FM-1"; missing version is empty.
std::string_view model_of(std::string_view identity);
std::string_view version_of(std::string_view identity);

}  // namespace fm1::identity
