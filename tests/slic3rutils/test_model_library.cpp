#include <catch2/catch_test_macros.hpp>
#include "slic3r/Utils/ModelLibrary/Library.hpp"
#include "slic3r/Utils/ModelLibrary/Schema.hpp"
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace Slic3r::ModelLibrary;

namespace {
struct Fixture {
    fs::path root;
    Fixture()
    {
        static std::atomic<unsigned> counter{0};
        root = fs::temp_directory_path() / ("porca-library-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + '-' + std::to_string(counter++));
        fs::create_directories(root);
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    fs::path file(const std::string& name, const std::string& data)
    {
        const auto path = root / fs::u8path(name);
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << data;
        return path;
    }
};
}

TEST_CASE("Library hashes preserve Strata's BLAKE3 identity", "[ModelLibrary]")
{
    Fixture fixture;
    CHECK(Library::hash_file(fixture.file("empty.stl", "")) == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    CHECK(Library::hash_file(fixture.file("abc.stl", "abc")) == "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
}

TEST_CASE("Opening an older Strata catalog preserves data and backs up before migration", "[ModelLibrary]")
{
    Fixture fixture;
    const auto root = fixture.root / "library"; fs::create_directory(root);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open((root / "library.db").u8string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, std::string(migrations[0]).c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "PRAGMA user_version=1; INSERT INTO models(id,name,dir,notes,added_at,updated_at) VALUES(1,'Existing model','models/existing','Personal note','2026-01-01','2026-01-01');", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    Library library(root);
    CHECK(library.detail(1)["notes"] == "Personal note");
    CHECK(library.detail(1)["ai_tagged_at"].is_null());
    REQUIRE(sqlite3_open((root / "library.db").u8string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&statement,nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int(statement,0) == 6);
    sqlite3_finalize(statement); sqlite3_close(db);
    size_t backups = 0; for (const auto& file : fs::directory_iterator(root / "backups")) if (file.is_regular_file()) ++backups;
    CHECK(backups >= 1);
}

TEST_CASE("Imports keep originals and preserve changed versions", "[ModelLibrary]")
{
    Fixture fixture;
    const auto source = fixture.file("downloads/Camera mount.stl", "first shape");
    Id first_id;
    {
        Library library(fixture.root / "library");
        const auto first = library.import(source);
        REQUIRE(first.status == "imported");
        first_id = first.model_id;
        CHECK(fs::exists(source));
        CHECK(library.import(source).status == "duplicate");
        CHECK(library.models().size() == 1);
        fixture.file("downloads/Camera mount.stl", "changed shape");
        const auto second = library.import(source);
        CHECK(second.status == "imported");
        CHECK(second.model_id != first_id);
        CHECK(library.models().size() == 2);
        const auto old_path = library.resolve(library.files(first_id)[0]["rel_path"]);
        CHECK(Library::hash_file(old_path) != Library::hash_file(source));
    }
    Library reopened(fixture.root / "library");
    CHECK(reopened.models().size() == 2);
    CHECK(reopened.detail(first_id)["name"] == "Camera mount");
}

TEST_CASE("Pack duplicates require every selected file in one model at its relative path", "[ModelLibrary]")
{
    Fixture fixture;
    fixture.file("pack/parts/part.stl", "shape");
    fixture.file("pack/README", "instructions");
    fixture.file("pack/source/design.scad", "source");
    Library library(fixture.root / "library");
    const auto first = library.import(fixture.root / "pack");
    REQUIRE(first.status == "imported");
    REQUIRE(library.files(first.model_id).size() == 3);
    CHECK(library.import(fixture.root / "pack").status == "duplicate");
    fixture.file("pack/README", "new instructions");
    CHECK(library.import(fixture.root / "pack").status == "imported");
    CHECK(library.models().size() == 2);
}

TEST_CASE("Search composes phrases operators exclusions and literal wildcard characters", "[ModelLibrary]")
{
    Fixture fixture;
    Library library(fixture.root / "library");
    const auto a = library.import(fixture.file("a.stl", "a"));
    const auto b = library.import(fixture.file("b.stl", "b"));
    REQUIRE(a.model_id != 0);
    REQUIRE(b.model_id != 0);
    library.update(a.model_id, {{"name", "Lens cap 100%"}, {"designer", "Ada"}, {"notes", "for camera"}});
    library.update(b.model_id, {{"name", "Lens cap test"}});
    library.tag({a.model_id}, "Camera", true);
    library.record_print(a.model_id, "good", "fits");
    CHECK(library.models({{"search", "\"lens cap\" -test tag:camera by:ada is:printed has:notes"}}).size() == 1);
    CHECK(library.models({{"search", "%"}}).size() == 1);
    CHECK(library.models({{"search", "is:unprinted"}}).size() == 1);
    CHECK(library.models({{"search", "-by:ada"}}).size() == 1);
    CHECK(library.models({{"search", "' OR 1=1 --"}}).empty());
}

TEST_CASE("Hidden rule tags and manual version decisions survive organization", "[ModelLibrary]")
{
    Fixture fixture;
    Library library(fixture.root / "library");
    const auto a = library.import(fixture.file("Camera mount v1.stl", "a"));
    const auto b = library.import(fixture.file("Camera mount v2.stl", "b"));
    REQUIRE(a.model_id != 0);
    REQUIRE(b.model_id != 0);
    CHECK(library.models({{"search", "tag:camera"}}).size() == 2);
    library.tag({a.model_id}, "Camera", false);
    library.unstack({a.model_id, b.model_id});
    library.organize();
    CHECK(library.models({{"search", "tag:camera"}}).size() == 1);
    CHECK(library.models({{"search", "is:stacked"}}).empty());
    library.stack({a.model_id, b.model_id});
    library.organize();
    CHECK(library.models({{"search", "is:stacked"}}).size() == 2);
}

TEST_CASE("Changed rules remove stale automatic tags and preserve user choices", "[ModelLibrary]")
{
    Fixture fixture;
    Library library(fixture.root / "library");
    const auto id = library.import(fixture.file("Camera mount.stl", "shape")).model_id;
    library.tag({id}, "Mounts", true);
    library.tag({id}, "Camera", false);
    library.set_setting("tag_rules", Json::array({{{"tag","Custom"},{"pattern","camera"},{"enabled",true}}}));
    library.organize();
    CHECK(library.models({{"search","tag:custom"}}).size() == 1);
    CHECK(library.models({{"search","tag:mounts"}}).size() == 1);
    CHECK(library.models({{"search","tag:camera"}}).empty());
    library.set_setting("tag_rules",Library::default_tag_rules()); library.organize();
    CHECK(library.models({{"search","tag:custom"}}).empty());
    CHECK(library.models({{"search","tag:mounts"}}).size() == 1);
    CHECK(library.models({{"search","tag:camera"}}).empty());
    const auto other = library.import(fixture.file("Plain component.stl", "another shape")).model_id;
    library.update(other,{{"source_url","https://example.invalid/download?sign=camera#mount"}}); library.organize();
    CHECK(library.detail(other)["tags"].empty());
}

TEST_CASE("Version grouping respects source designers and physical size while allowing gradual revisions", "[ModelLibrary]")
{
    Fixture fixture;
    Library library(fixture.root / "library"); library.set_setting("auto_stack",false);
    const auto add = [&](int version, double size, const std::string& designer) {
        const auto id = library.import(fixture.file("Camera mount v"+std::to_string(version)+".stl",std::to_string(version)),
            [size](const fs::path&,const std::string&,const fs::path&) { return Json{{"size_x",size},{"size_y",size},{"size_z",size}}; }).model_id;
        library.update(id,{{"designer",designer}}); return id;
    };
    const auto a = add(1,10,"Ada"), b = add(2,14,"ada"), c = add(3,19,"Ada");
    const auto large = add(4,40,"Ada"), other = add(5,14,"Grace");
    library.set_setting("auto_stack",true); library.organize();
    REQUIRE_FALSE(library.detail(a)["stack_id"].is_null());
    CHECK(library.detail(a)["stack_id"] == library.detail(b)["stack_id"]);
    CHECK(library.detail(a)["stack_id"] == library.detail(c)["stack_id"]);
    CHECK(library.detail(large)["stack_id"].is_null());
    CHECK(library.detail(other)["stack_id"].is_null());
}

TEST_CASE("Name families count designs once and keep their labels when filtered", "[ModelLibrary]")
{
    CHECK(Library::version_stem("opengrid2x8 rev12.stl") == "opengrid 2x8");
    CHECK(Library::version_stem("x2d bracket v12b") == "x2d bracket");
    Fixture fixture;
    Library library(fixture.root / "library"); library.set_setting("auto_stack",false);
    const auto a = library.import(fixture.file("Lens cap 49mm.stl","a")).model_id;
    const auto b = library.import(fixture.file("Lens cap 58mm.stl","b")).model_id;
    const auto c = library.import(fixture.file("Lens cap 72mm.stl","c")).model_id;
    const auto version = library.import(fixture.file("Lens cap 49mm v2.stl","version")).model_id;
    library.stack({a,version});
    auto rows = library.models({{"families",true},{"search","49mm"}});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]["family"] == "Lens cap");
    CHECK(rows[1]["family"] == "Lens cap");
    library.stack({a,b,version});
    rows = library.models({{"families",true},{"id",c}});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["family"] == "");
}

TEST_CASE("Collections and print reviews survive reopening", "[ModelLibrary]")
{
    Fixture fixture;
    Id id, collection, print;
    {
        Library library(fixture.root / "library");
        id = library.import(fixture.file("model.stl", "shape")).model_id;
        collection = library.create_collection("Desk");
        library.collect(collection, {id}, true);
        print = library.record_print(id, "failed", "detached");
        library.update_print(print, "meh", "one usable part");
        library.update(id, {{"notes", "Keep at 100%"}, {"favorite", true}});
    }
    Library library(fixture.root / "library");
    CHECK(library.models({{"collection", collection}}).size() == 1);
    CHECK(library.prints(id)[0]["outcome"] == "meh");
    CHECK(library.detail(id)["notes"] == "Keep at 100%");
    CHECK(library.models({{"favorites", true}}).size() == 1);
    library.archive({id}, true);
    CHECK(library.models().empty());
    CHECK(library.models({{"search", "is:archived"}}).size() == 1);
}

TEST_CASE("Cleanup rechecks both copies and never trashes unknown pack contents", "[ModelLibrary]")
{
    Fixture fixture;
    const auto source = fixture.file("downloads/part.stl", "shape");
    Library library(fixture.root / "library");
    library.add_watch(source.parent_path(), true);
    REQUIRE(library.import(source).status == "imported");
    REQUIRE(library.leftovers().size() == 1);
    fixture.file("downloads/part.stl", "changed");
    bool trashed = false;
    CHECK_THROWS(library.trash_leftovers({source}, [&](const fs::path&) { trashed = true; return true; }));
    CHECK_FALSE(trashed);
    CHECK(fs::exists(source));
    fixture.file("downloads/pack/part.stl", "another shape");
    fixture.file("downloads/pack/unknown.xyz", "must keep");
    REQUIRE(library.import(source.parent_path() / "pack").status == "imported");
    CHECK(library.leftovers().empty());
}

TEST_CASE("Library rejects traversal and newer catalogs without changing their version", "[ModelLibrary]")
{
    Fixture fixture;
    {
        Library library(fixture.root / "library");
        CHECK_THROWS(library.resolve("../outside"));
        CHECK_THROWS(library.resolve("/outside"));
        CHECK_THROWS(library.resolve("models/../../outside"));
    }
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open((fixture.root / "library/library.db").u8string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "PRAGMA user_version=999", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    CHECK_THROWS(Library(fixture.root / "library"));
    REQUIRE(sqlite3_open((fixture.root / "library/library.db").u8string().c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(sqlite3_column_int(statement, 0) == 999);
    sqlite3_finalize(statement);
    sqlite3_close(db);
}

TEST_CASE("Merging and splitting packs retains files tags collections and print history", "[ModelLibrary]")
{
    Fixture fixture;
    Library library(fixture.root / "library");
    const auto a = library.import(fixture.file("Pack A/part.stl", "a").parent_path());
    const auto b = library.import(fixture.file("Pack B/part.stl", "b").parent_path());
    library.tag({b.model_id}, "Useful", true);
    const Id collection = library.create_collection("Tools");
    library.collect(collection, {b.model_id}, true);
    library.record_print(b.model_id, "good", "works");
    library.merge(a.model_id, {b.model_id});
    CHECK(library.models().size() == 1);
    CHECK(library.files(a.model_id).size() == 2);
    CHECK(library.prints(a.model_id).size() == 1);
    CHECK(library.models({{"collection", collection}}).size() == 1);
    const auto split = library.split(a.model_id, "Pack B");
    CHECK(library.files(split).size() == 1);
    CHECK(fs::exists(library.resolve(library.files(split)[0]["rel_path"])));
    CHECK(library.models({{"collection", collection}}).size() == 2);
}

TEST_CASE("Growing packs retain identity and edits preserve old files", "[ModelLibrary]")
{
    Fixture fixture;
    fixture.file("pack/a.stl", "first");
    Library library(fixture.root / "library");
    const auto first = library.import(fixture.root / "pack");
    REQUIRE(first.status == "imported");
    library.update(first.model_id, {{"notes", "keep my annotation"}});
    fixture.file("pack/parts/b.stl", "second");
    const auto added = library.import(fixture.root / "pack");
    REQUIRE(added.status == "imported");
    CHECK(added.model_id == first.model_id);
    CHECK(library.files(first.model_id).size() == 2);
    CHECK(library.detail(first.model_id)["notes"] == "keep my annotation");
    CHECK(library.models().size() == 1);
    fixture.file("pack/a.stl", "edited");
    const auto edited = library.import(fixture.root / "pack");
    CHECK(edited.model_id != first.model_id);
    CHECK(library.files(first.model_id).size() == 2);
    CHECK(library.models().size() == 2);
}

TEST_CASE("Merges preserve conflicting annotations and disabled stacking stays disabled", "[ModelLibrary]")
{
    Fixture fixture;
    Library library(fixture.root / "library");
    library.set_setting("auto_stack", false);
    const auto a = library.import(fixture.file("Camera holder v1.stl", "one")).model_id;
    const auto b = library.import(fixture.file("Camera holder v2.stl", "two")).model_id;
    CHECK(library.models({{"search", "is:stacked"}}).empty());
    library.update(a, {{"notes", "first notes"}, {"license", "first license"}});
    library.update(b, {{"notes", "second notes"}, {"license", "second license"}, {"designer", "Ada"}});
    library.merge(a, {b});
    const auto merged = library.detail(a);
    const std::string notes = merged["notes"];
    CHECK(notes.find("first notes") != std::string::npos);
    CHECK(notes.find("second notes") != std::string::npos);
    CHECK(notes.find("second license") != std::string::npos);
    CHECK(merged["license"] == "first license");
    CHECK(merged["designer"] == "Ada");
}

#include "slic3r/Utils/ModelLibrary/TextMetadata.hpp"
#include "slic3r/Utils/ModelLibrary/Online.hpp"
#include <miniz/miniz.h>

TEST_CASE("STEP and repository text retain names authors and readable descriptions", "[ModelLibrary]")
{
    const auto step = step_metadata("ISO-10303-21; HEADER; FILE_DESCRIPTION(('A fitting'),'2;1'); FILE_NAME('C:\\\\parts\\\\Bracket.step','today',('Ren\\X2\\00E9\\X0\\'),('Lab'),'CAD','System',''); FILE_SCHEMA(('AP214')); ENDSEC;");
    CHECK(step["title"] == "Bracket");
    CHECK(step["designer"] == "René");
    CHECK(step["meta"].size() == 8);
    const auto readme = readme_metadata("Desk Hook by Ada on Thingiverse: https://www.thingiverse.com/thing:42\nDesk Hook by Ada is licensed under the CC BY license.\n");
    CHECK(readme["title"] == "Desk Hook"); CHECK(readme["designer"] == "Ada"); CHECK(readme["license"] == "CC BY");
    CHECK(readme["source_url"] == "https://www.thingiverse.com/thing:42");
    CHECK(plain_text("&lt;p&gt;A &amp;amp; B&lt;/p&gt;<script>bad()</script>") == "A & B");
    CHECK_THROWS(step_metadata("not a STEP file"));
}

TEST_CASE("G-code metadata reads both text and binary comment blocks", "[ModelLibrary]")
{
    const auto gcode = gcode_metadata("; printer_model = MK4\n; filament_type = PETG\n; total estimated time: 10m\n");
    CHECK(gcode.fields["meta"].size() == 3);
    std::vector<std::uint8_t> binary{'G','C','D','E',1,0,0,0,0,0};
    const std::string payload = "printer_model=MK4\nfilament_type=PETG\n";
    binary.insert(binary.end(), {3,0,0,0, std::uint8_t(payload.size()),0,0,0, 0,0});
    binary.insert(binary.end(),payload.begin(),payload.end());
    const auto parsed = bgcode_metadata(binary);
    CHECK(parsed.fields["meta"].size() == 2);
    CHECK(parsed.fields["meta"][0]["value"] == "MK4");
    CHECK_THROWS(bgcode_metadata({1,2,3}));
}

TEST_CASE("Online tagging validates replies and preserves deliberately removed tags", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    const auto id = library.import(fixture.file("Toy.stl","shape")).model_id;
    library.set_setting("ai_tagging",true); library.tag({id},"Toys",false);
    unsigned calls = 0;
    const WebRequest web = [&](const std::string& url, const Json& body, const auto& headers, const Cancel&) {
        ++calls; CHECK(url == "https://api.anthropic.com/v1/messages"); CHECK(headers.at("x-api-key") == "fixture-only");
        CHECK(body["model"] == "claude-haiku-4-5");
        const Json answer{{"models",Json::array({{{"id",id},{"tags",{"Toys","Not a permitted tag","Home"}}},{{"id",999999},{"tags",{"Tools"}}}})}};
        return WebReply{200,Json{{"stop_reason","end_turn"},{"content",Json::array({{{"type","text"},{"text",answer.dump()}}})}}.dump()};
    };
    tag_with_ai(library,"fixture-only",web);
    CHECK(calls == 1); CHECK(library.models({{"search","tag:toys"}}).empty());
    CHECK(library.models({{"search","tag:home"}}).size() == 1);
    CHECK(!library.detail(id)["ai_tagged_at"].is_null());
    tag_with_ai(library,"fixture-only",web); CHECK(calls == 1);
    CHECK_THROWS(ai_answers({{"stop_reason","max_tokens"}}, {id}, {"Toys"}));
}

TEST_CASE("Page details fill missing fields without replacing personal edits", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    const auto id = library.import(fixture.file("Downloaded thing.stl","shape")).model_id;
    library.update(id,{{"name","My chosen name"},{"designer","Personal attribution"},{"source_url","https://www.printables.com/model/123-test"}});
    unsigned calls = 0;
    const WebRequest web = [&](const std::string& url, const Json& body, const auto&, const Cancel&) {
        ++calls; CHECK(url == "https://api.printables.com/graphql/"); CHECK(body["variables"]["id"] == "123");
        return WebReply{200,R"({"data":{"print":{"name":"Page name","user":{"publicUsername":"Page designer"},"license":{"name":"CC BY"},"description":"<p>How to print</p>","images":[]}}})"};
    };
    enrich_pages(library,web); CHECK(calls == 0);
    library.set_setting("page_details",true); enrich_pages(library,web); CHECK(calls == 1);
    const auto model = library.detail(id);
    CHECK(model["name"] == "My chosen name"); CHECK(model["designer"] == "Personal attribution"); CHECK(model["license"] == "CC BY"); CHECK(model["description"] == "How to print");
    CHECK(!model["page_read_at"].is_null());
    CHECK(page_details("https://printables.com.attacker.invalid/model/123",web).empty()); CHECK(calls == 1);
}

TEST_CASE("ZIP cleanup includes unsupported and hidden contents in its proof", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    const auto downloads = fixture.root / "downloads"; fs::create_directories(downloads); library.add_watch(downloads,true);
    const auto archive = downloads / "pack.zip";
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip,archive.u8string().c_str(),0));
    REQUIRE(mz_zip_writer_add_mem(&zip,"part.stl","shape",5,MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&zip,".git/config","private",7,MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip)); mz_zip_writer_end(&zip);
    REQUIRE(library.import(archive).status == "imported");
    CHECK(library.leftovers().empty());
    bool trashed = false;
    CHECK_THROWS(library.trash_leftovers({archive},[&](const auto&) { trashed = true; return true; }));
    CHECK_FALSE(trashed); CHECK(fs::exists(archive));
}

#include "slic3r/Utils/ModelLibrary/Watch.hpp"
#include "slic3r/Utils/ModelLibrary/Source.hpp"

TEST_CASE("Folder watching waits for stable downloads and remembers completed imports", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    const auto downloads = fixture.root / "downloads"; fs::create_directories(downloads); library.add_watch(downloads,true);
    const auto source = fixture.file("downloads/new.stl","shape");
    // Keep this test independent of timestamp rounding around a UTC second.
    fs::last_write_time(source,fs::file_time_type::clock::now() + std::chrono::seconds(1));
    Watch watch; const auto now = std::chrono::steady_clock::now();
    CHECK(watch.poll(library,{}, {}, now).empty()); CHECK(library.models().empty());
    CHECK(watch.poll(library,{}, {}, now + std::chrono::seconds(1)).empty());
    CHECK(watch.poll(library,{}, {}, now + std::chrono::seconds(3)).size() == 1);
    CHECK(library.models().size() == 1);
    CHECK(watch.poll(library,{}, {}, now + std::chrono::seconds(6)).empty());
    fixture.file("downloads/new.stl","changed shape");
    CHECK(watch.poll(library,{}, {}, now + std::chrono::seconds(8)).empty());
    CHECK(watch.poll(library,{}, {}, now + std::chrono::seconds(11)).size() == 1);
    CHECK(library.models().size() == 2);
}

TEST_CASE("Folder watching notices renamed pack files with unchanged size and timestamps", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    const auto downloads = fixture.root / "downloads"; fs::create_directories(downloads);
    library.add_watch(downloads, true);
    const auto original = fixture.file("downloads/pack/original.stl", "shape");
    const auto stamp = fs::file_time_type::clock::now() + std::chrono::seconds(1);
    fs::last_write_time(original, stamp);
    Watch watch; const auto now = std::chrono::steady_clock::now();
    CHECK(watch.poll(library, {}, {}, now).empty());
    const auto first = watch.poll(library, {}, {}, now + std::chrono::seconds(5));
    REQUIRE(first.size() == 1);
    const auto renamed = original.parent_path() / "renamed.stl";
    fs::rename(original, renamed);
    const bool timestamp_preserved = fs::last_write_time(renamed) == stamp;
    REQUIRE(timestamp_preserved);
    CHECK(watch.poll(library, {}, {}, now + std::chrono::seconds(6)).empty());
    const auto changed = watch.poll(library, {}, {}, now + std::chrono::seconds(11));
    REQUIRE(changed.size() == 1);
    CHECK(library.models().size() == 2);
    CHECK(library.files(first[0]["model_id"])[0]["original_name"] == "original.stl");
    CHECK(library.files(changed[0]["model_id"])[0]["original_name"] == "renamed.stl");
}

TEST_CASE("Slicer download discovery reads checksummed configs and deduplicates folders", "[ModelLibrary]")
{
    Fixture fixture; const auto downloads = fixture.root / "downloads"; fs::create_directories(downloads);
    const auto value = Json(downloads.u8string()).dump();
    fixture.file("config/BambuStudio/BambuStudio.conf", "{\"app\":{\"download_path\":" + value + "}}\n# MD5 checksum ABC\n");
    fixture.file("config/OrcaSlicer/OrcaSlicer.conf", "{\"app\":{\"download_path\":" + value + "}}");
    const auto paths = slicer_download_folders(fixture.root / "config");
    REQUIRE(paths.size() == 1); CHECK(paths.front() == fs::canonical(downloads));
}

TEST_CASE("Restoring old flat packs preserves annotations and refuses changed originals", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    const auto archive = fixture.root / "pack.zip";
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_file(&zip,archive.u8string().c_str(),0));
    REQUIRE(mz_zip_writer_add_mem(&zip,"parts/widget.stl","shape",5,MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&zip,"docs/README","instructions",12,MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip)); mz_zip_writer_end(&zip);
    const auto result = library.import(archive); REQUIRE(result.status == "imported");
    library.update(result.model_id,{{"notes","Keep these notes"}});
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open((library.root() / "library.db").u8string().c_str(),&db) == SQLITE_OK);
    for (const auto& file : library.files(result.model_id)) {
        const auto old = library.resolve(file["rel_path"]);
        const auto next = library.detail(result.model_id)["dir"].get<std::string>() + '/' + file["original_name"].get<std::string>();
        fs::rename(old,library.resolve(next));
        const auto sql = "UPDATE files SET rel_path='" + next + "' WHERE id=" + std::to_string(file["id"].get<Id>());
        REQUIRE(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK);
    }
    sqlite3_close(db);
    library.restore_pack_layout(result.model_id);
    const auto dir = library.detail(result.model_id)["dir"].get<std::string>();
    CHECK(fs::exists(library.resolve(dir + "/parts/widget.stl")));
    CHECK(fs::exists(library.resolve(dir + "/docs/README")));
    CHECK_FALSE(fs::exists(library.resolve(dir + "/widget.stl")));
    CHECK(library.detail(result.model_id)["notes"] == "Keep these notes");
    fixture.file("pack.zip","changed download");
    CHECK_THROWS(library.restore_pack_layout(result.model_id));
    CHECK(fs::exists(library.resolve(dir + "/parts/widget.stl")));
}

TEST_CASE("Organized imports preview verified packs versions and omitted files", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    fixture.file("collection/Tools/Camera clip/parts/clip.stl","shape");
    fixture.file("collection/Tools/Camera clip/docs/README","instructions");
    fixture.file("collection/Tools/Camera clip/unknown.bin","keep original");
    fixture.file("collection/Tools/unsupported.xyz","unknown");
    REQUIRE(library.import(fixture.root / "collection/Tools/Camera clip").status == "imported");
    auto plan = library.plan_organized(fixture.root / "collection");
    REQUIRE(plan.size() == 1); REQUIRE(plan[0]["items"].size() == 2);
    CHECK(plan[0]["items"][0]["status"] == "duplicate");
    CHECK(plan[0]["items"][0]["left_out"] == Json::array({"unknown.bin"}));
    CHECK(plan[0]["items"][1]["status"] == "unsupported");
    auto result = library.import_organized(fixture.root / "collection",{"Tools"});
    REQUIRE(result.size() == 1); CHECK(result[0]["status"] == "duplicate");
    CHECK(library.models({{"search","in:Tools"}}).size() == 1);
    fixture.file("collection/Tools/Camera clip/parts/clip.stl","edited shape");
    plan = library.plan_organized(fixture.root / "collection");
    CHECK(plan[0]["items"][0]["status"] == "version");
    CHECK(library.models().size() == 1);
    CHECK(fs::exists(fixture.root / "collection/Tools/Camera clip/unknown.bin"));
}

TEST_CASE("Overlapping packs stack without disturbing manual groups", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    fixture.file("First/one.stl","one"); fixture.file("First/two.stl","two"); fixture.file("First/three.stl","three");
    fixture.file("Second/one.stl","one"); fixture.file("Second/two.stl","two"); fixture.file("Second/four.stl","four");
    const auto first = library.import(fixture.root / "First");
    const auto second = library.import(fixture.root / "Second");
    REQUIRE(first.status == "imported"); REQUIRE(second.status == "imported");
    REQUIRE_FALSE(library.detail(first.model_id)["stack_id"].is_null());
    CHECK(library.detail(first.model_id)["stack_id"] == library.detail(second.model_id)["stack_id"]);
    library.unstack({second.model_id}); library.organize();
    CHECK(library.detail(second.model_id)["stack_id"].is_null());
}

TEST_CASE("Background geometry updates are bounded and do not retry excluded models forever", "[ModelLibrary]")
{
    Fixture fixture; Library library(fixture.root / "library");
    const auto first = library.import(fixture.file("First.stl","shape one"));
    const auto second = library.import(fixture.file("Second.stl","shape two"));
    unsigned reads = 0;
    const Extractor excluded = [&](const auto&,const auto&,const auto&) { ++reads; return Json::object(); };
    CHECK(library.backfill_geometry(excluded)); CHECK(reads == 1);
    CHECK(library.backfill_geometry(excluded)); CHECK(reads == 2);
    CHECK_FALSE(library.backfill_geometry(excluded)); CHECK(reads == 2);
    CHECK(library.detail(first.model_id)["files"][0]["geometry_hash"].is_null());
    CHECK(library.detail(second.model_id)["files"][0]["geometry_hash"].is_null());
}

#ifdef PORCA_LIBRARY_FORMAT_TESTS
#include "slic3r/Utils/ModelLibrary/Extract.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include <catch2/matchers/catch_matchers_floating_point.hpp>

TEST_CASE("Library mesh readers preserve equivalent geometry across portable formats", "[ModelLibraryFormats]")
{
    Fixture fixture;
    const auto obj = fixture.file("triangle.obj", "v 0 0 0\nv 10 0 0\nv 0 10 0\nf 1 2 3\n");
    const auto ply = fixture.file("triangle.ply", "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n10 0 0\n0 10 0\n3 0 1 2\n");
    const auto stl = fixture.file("triangle.stl", "solid triangle\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 10 0 0\nvertex 0 10 0\nendloop\nendfacet\nendsolid triangle\n");
    const auto amf = fixture.file("triangle.amf", "<amf unit=\"millimeter\"><object id=\"0\"><mesh><vertices><vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex><vertex><coordinates><x>10</x><y>0</y><z>0</z></coordinates></vertex><vertex><coordinates><x>0</x><y>10</y><z>0</z></coordinates></vertex></vertices><volume><triangle><v1>0</v1><v2>1</v2><v3>2</v3></triangle></volume></mesh></object></amf>");
    const auto expected = geometry_fingerprint(read_mesh(obj).its);
    REQUIRE_FALSE(expected.empty());
    for (const auto& path : {stl,ply,amf}) {
        CAPTURE(path);
        const auto mesh = read_mesh(path);
        CHECK(mesh.its.indices.size() == 1);
        CHECK(geometry_fingerprint(mesh.its) == expected);
    }
}

TEST_CASE("Library 3MF previews preserve component transforms and repeated instances", "[ModelLibraryFormats]")
{
    Fixture fixture;
    const auto path = fixture.root / "instances.3mf";
    const std::string model = R"(<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/><vertex x="0" y="10" z="0"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles></mesh></object><object id="2"><components><component objectid="1" transform="1 0 0 0 1 0 0 0 1 20 0 0"/></components></object></resources><build><item objectid="1"/><item objectid="2"/></build></model>)";
    mz_zip_archive zip{}; REQUIRE(mz_zip_writer_init_file(&zip,path.u8string().c_str(),0));
    REQUIRE(mz_zip_writer_add_mem(&zip,"3D/3dmodel.model",model.data(),model.size(),MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip)); mz_zip_writer_end(&zip);
    const auto mesh = read_mesh(path);
    CHECK(mesh.its.indices.size() == 2);
    CHECK_THAT(mesh.bounding_box().size().x(),Catch::Matchers::WithinAbs(30,0.0001));
    CHECK_THAT(mesh.bounding_box().size().y(),Catch::Matchers::WithinAbs(10,0.0001));
}

TEST_CASE("Library previews reject invalid references before constructing meshes", "[ModelLibraryFormats]")
{
    Fixture fixture;
    const auto bad = fixture.file("bad.amf", "<amf><object id=\"0\"><mesh><vertices><vertex><coordinates><x>0</x><y>0</y><z>0</z></coordinates></vertex></vertices><volume><triangle><v1>0</v1><v2>1</v2><v3>99999</v3></triangle></volume></mesh></object></amf>");
    CHECK_THROWS(read_mesh(bad));
    const auto cube = Slic3r::make_cube(20,20,20);
    const auto image = render_mesh(cube.its,128);
    const auto path = fixture.root / "preview.png";
    REQUIRE(Slic3r::png::write_rgb_to_file(path.u8string(),128,128,image));
    const auto preview = read_preview(path,64);
    CHECK(preview.width == 64); CHECK(preview.height == 64); CHECK(preview.rgb.size() == 64 * 64 * 3);
}
#endif
