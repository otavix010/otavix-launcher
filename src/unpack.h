#pragma once
// Unpacking an archive with the system's tools. Windows 10 and 11 carry libarchive's tar.exe, which reads .zip, .tar.gz, 7z
// and rar. On Linux libarchive's tar is "bsdtar", which some distributions leave out (Ubuntu and Debian: libarchive-tools;
// report 1367): there a .tar.gz / .tar goes to GNU tar and a .zip to unzip, and the error names the package to install.

#include <string>
#include <vector>

namespace unpack {

// the archive tool for listing and unpacking any format: tar.exe on Windows, bsdtar on Linux ("" when it is not installed)
std::string Tar();

// unpacks archive into dest (which exists); false with why (in the interface's language) when no tool could do it
bool Extract(const std::string& archive, const std::string& dest, std::string& why);

// packs the folders names (relative to parent) into out as a .tar.gz; false with why when it failed (tar.exe on
// Windows; bsdtar or GNU tar on Linux, both make .tar.gz)
bool PackTarGz(const std::string& parent, const std::vector<std::string>& names, const std::string& out, std::string& why);

// the hint for a Linux PC without bsdtar, in the interface's language (empty on Windows)
std::string MissingBsdtarHint();

}  // namespace unpack
