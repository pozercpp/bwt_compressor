#pragma once
#include <archive/password.hpp>
#include <optional>
namespace cli {
void read_password(archive::Password&, bool creating, std::optional<int> fd);
}
