#include <catch2/catch_test_macros.hpp>

#include "libslic3r/PrintDeliveryValidation.hpp"
#include "test_utils.hpp"

#include <boost/nowide/fstream.hpp>

#include <ios>
#include <string>

using namespace Slic3r;

TEST_CASE("Flashforge delivery requires one confirmed model across the saved printer plate and slice", "[PrintDelivery]")
{
    const PrintDeliveryIdentity ad5m{"Flashforge Adventurer 5M", "0.4"};
    const PrintDeliveryIdentity ad5x{"Flashforge AD5X", "0.25"};
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5m, ad5m, ad5m, ad5m, true)
          == PrintDeliveryIssue::none);
    CHECK(check_flashforge_delivery("Flashforge", ad5x, ad5x, ad5x, ad5x, true)
          == PrintDeliveryIssue::unknown_destination_model);
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5x, ad5x, ad5x, ad5x, true)
          == PrintDeliveryIssue::destination_profile_mismatch);
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5m, ad5x, ad5x, ad5x, true)
          == PrintDeliveryIssue::plate_profile_mismatch);
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5m, ad5m, ad5x, ad5m, true)
          == PrintDeliveryIssue::editing_profile_mismatch);
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5m, ad5m, ad5m, ad5x, true)
          == PrintDeliveryIssue::slice_profile_mismatch);
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5m,
          {ad5m.model, ad5m.nozzle, "AD5M profile A"}, ad5m,
          {ad5m.model, ad5m.nozzle, "AD5M profile B"}, true)
          == PrintDeliveryIssue::slice_profile_mismatch);
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5m, ad5m, ad5m, ad5m, false)
          == PrintDeliveryIssue::slice_not_ready);
    CHECK(check_flashforge_delivery("Flashforge Adventurer 5M", ad5m, ad5m, ad5m, {}, true)
          == PrintDeliveryIssue::slice_identity_unavailable);
    CHECK_FALSE(confirmed_flashforge_profile("Flashforge", ad5x.model));
    CHECK_FALSE(confirmed_flashforge_profile("Flashforge Adventurer 5M", ad5x.model));
    CHECK(confirmed_flashforge_profile("Flashforge Adventurer 5M", ad5m.model));
}

TEST_CASE("Multi-extruder slices match a single nozzle variant on every extruder", "[PrintDelivery]")
{
    const PrintDeliveryIdentity profile{"Flashforge Creator 5", "0.4", "Creator 5 0.4 nozzle"};
    const auto sliced = [&](const std::string& nozzles) {
        return PrintDeliveryIdentity{profile.model, nozzles, profile.preset};
    };
    CHECK(check_flashforge_delivery(profile.model, profile, profile, profile, sliced("0.4,0.4"), true)
          == PrintDeliveryIssue::none);
    CHECK(check_flashforge_delivery(profile.model, profile, profile, profile, sliced("0.40,0.4"), true)
          == PrintDeliveryIssue::none);
    CHECK(check_flashforge_delivery(profile.model, profile, profile, profile, sliced("0.6,0.6"), true)
          == PrintDeliveryIssue::slice_profile_mismatch);
    CHECK(check_flashforge_delivery(profile.model, profile, profile, profile, sliced("0.4,0.6"), true)
          == PrintDeliveryIssue::slice_profile_mismatch);
    CHECK(check_flashforge_delivery(profile.model, profile, profile, profile, sliced("0.4,"), true)
          == PrintDeliveryIssue::slice_profile_mismatch);
}

TEST_CASE("Delivery reads the selected G-code footer rather than another plate's settings", "[PrintDelivery]")
{
    // A non-ASCII file name exercises the UTF-8 path used for Windows profiles.
    ScopedTemporaryDir directory("porca-delivery");
    const std::string path = directory.string() + "/d\xC3\xA9livery identity.gcode";
    {
        boost::nowide::ofstream file(path.c_str(), std::ios::binary);
        REQUIRE(file.good());
        file << "; printer_model = Flashforge Adventurer 5M\n";
        file << "G1 X0 Y0\n";
        file << "; nozzle_diameter = 0.25\n";
        file << "; printer_model = Flashforge AD5X\n";
        file << "; printer_settings_id = Flashforge AD5X 0.25 nozzle\n";
    }
    const auto sliced = read_gcode_delivery_identity(path);
    CHECK(sliced.model == "Flashforge AD5X");
    CHECK(sliced.nozzle == "0.25");
    CHECK(sliced.preset == "Flashforge AD5X 0.25 nozzle");
    const PrintDeliveryIdentity ad5m{"Flashforge Adventurer 5M", "0.4"};
    CHECK(check_flashforge_delivery(ad5m.model, ad5m, ad5m, ad5m, sliced, true)
          == PrintDeliveryIssue::slice_profile_mismatch);
    CHECK(read_gcode_delivery_identity(directory.string() + "/missing.gcode").model.empty());
}
