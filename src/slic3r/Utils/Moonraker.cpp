#include "Moonraker.hpp"

#include <exception>
#include <cstddef>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <wx/string.h>
#include <utility>
#include <wx/arrstr.h>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {

Moonraker::Moonraker(DynamicPrintConfig *config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_cafile(config->opt_string("printhost_cafile"))
    , m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{
    m_snapmaker_u1 = is_snapmaker_u1_config(*config);
}

bool Moonraker::is_snapmaker_u1_config(const DynamicPrintConfig& config)
{
    const auto* model = config.option<ConfigOptionString>("printer_model");
    const auto* host = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return model && model->value == "Snapmaker U1" && host && host->value == htMoonraker;
}

const char* Moonraker::get_name() const { return "Moonraker"; }

wxString Moonraker::get_test_ok_msg() const
{
    return _(L("Connection to Moonraker is working correctly."));
}

wxString Moonraker::get_test_failed_msg(wxString &msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to Moonraker"), msg);
}

std::string Moonraker::make_url(const std::string &path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/')
            return (boost::format("%1%%2%") % m_host % path).str();
        return (boost::format("%1%/%2%") % m_host % path).str();
    }
    return (boost::format("http://%1%/%2%") % m_host % path).str();
}

void Moonraker::set_auth(Http &http) const
{
    if (!m_apikey.empty())
        http.header("X-Api-Key", m_apikey);
    if (!m_cafile.empty())
        http.ca_file(m_cafile);
}

bool Moonraker::test(wxString &msg) const
{
    //ORCA: Moonraker's /server/info returns
    //          { "result": { "klippy_state": "ready|startup|shutdown|error|disconnected", ... } }
    //      We treat the connection as healthy as long as the envelope is valid and `klippy_state`
    //      is present — matching the OctoPrint/PrusaLink convention of "can I reach this host?".
    //      Klipper state (idle, error, etc.) is surfaced to the log but does not gate the test:
    //      buddy-fork firmwares legitimately report non-`ready` states at idle, and any real upload
    //      problem will surface a contextual error at upload() time anyway.
    const char *name = get_name();
    bool res = true;
    auto url = make_url("server/info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get server info at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting server info: %2%, HTTP %3%, body: `%4%`")
            % name % error % status % body;
        res = false;
        msg = format_error(body, error, status);
    })
    .on_complete([&](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/info body: %2%") % name % body;
        try {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);

            const auto klippy_state = ptree.get_optional<std::string>("result.klippy_state");
            if (!klippy_state) {
                //ORCA: response wasn't shaped like a Moonraker /server/info reply — likely an OctoPrint
                //      or PrusaLink host the user mis-selected as Moonraker, or a totally different
                //      service. Treat as a connection failure with a clear hint.
                res = false;
                msg = _L("The host responded but it doesn't look like Moonraker (missing result.klippy_state).");
                return;
            }
            BOOST_LOG_TRIVIAL(info) << boost::format("%1%: klippy_state = %2%") % name % (*klippy_state);
        } catch (const std::exception &ex) {
            res = false;
            msg = GUI::format_wxstr(_L("Could not parse Moonraker server response: %s"), ex.what());
        }
    })
#ifdef WIN32
    .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
    .perform_sync();

    return res;
}

bool Moonraker::get_storage(wxArrayString &storage_path, wxArrayString &storage_name) const
{
    //ORCA: GET /server/files/roots enumerates Moonraker's storage roots (default "gcodes" plus any
    //      configured extras like "config", "logs", "timelapse"). Only roots with permissions
    //      including "rw" or "rwd" can receive uploads; we filter to those so the UI dropdown only
    //      offers usable destinations. The base class returns false (no per-host storage); returning
    //      true here populates the storage picker in PrintHostDialogs's send-to-print dialog.
    //      Failures (404 — older Moonraker, or a buddy-fork that doesn't implement the endpoint)
    //      gracefully degrade to false so upload() falls back to the hardcoded "gcodes" default.
    const char *name = get_name();
    bool got_any = false;
    auto url = make_url("server/files/roots");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Enumerating storage roots at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        //ORCA: /server/files/roots is optional in the Moonraker spec and absent on older versions
        //      and slimmer shims (e.g. Prusa-Firmware-Buddy 0.8.x prusalink-shim returns 501). A
        //      missing endpoint here is benign — upload() silently falls back to the hardcoded
        //      "gcodes" root — so don't pollute the log at warning level for it. Other HTTP
        //      errors still warn.
        if (status == 404 || status == 501) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots not implemented (HTTP %2%); upload() will fall back to the \"gcodes\" root.")
                % name % status;
        } else {
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not enumerate roots: %2%, HTTP %3%, body: `%4%`")
                % name % error % status % body;
        }
    })
    .on_complete([&](std::string body, unsigned) {
        BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: /server/files/roots body: %2%") % name % body;
        try {
            std::stringstream ss(body);
            pt::ptree ptree;
            pt::read_json(ss, ptree);
            const auto result_node = ptree.get_child_optional("result");
            if (!result_node)
                return;
            for (const auto &child : *result_node) {
                const std::string &root = child.second.get<std::string>("name", "");
                const std::string &perms = child.second.get<std::string>("permissions", "");
                if (root.empty() || perms.find('w') == std::string::npos)
                    continue;
                storage_path.Add(wxString::FromUTF8(root));
                storage_name.Add(wxString::FromUTF8(root));
                got_any = true;
            }
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Could not parse roots: %2%") % name % ex.what();
        }
    })
#ifdef WIN32
    .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
    .perform_sync();

    return got_any;
}

bool Moonraker::start_print(wxString &error_msg, const std::string &filename) const
{
    //ORCA: POST /printer/print/start with JSON body { "filename": "<name>.gcode" }.
    //      `filename` is the item.path /server/files/upload returned (the storage-relative
    //      path inside `root`, no leading slash, with extension), or the original filename
    //      when the reply had none. Build the body via property_tree so that special characters
    //      in the filename (quotes, backslashes) are properly escaped.
    const char *name = get_name();
    bool res = true;
    auto url = make_url("printer/print/start");
    pt::ptree body_tree;
    body_tree.put("filename", filename);
    std::ostringstream body_ss;
    pt::write_json(body_ss, body_tree, /*pretty=*/false);
    std::string body = body_ss.str();

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Starting print of %2% at %3%") % name % filename % url;

    auto http = Http::post(std::move(url));
    set_auth(http);
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: print/start HTTP %2%: %3%") % name % status % body;
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error starting print at %2%: %3%, HTTP %4%, body: `%5%`")
                % name % url % error % status % body;
            res = false;
            error_msg = format_error(body, error, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    return res;
}

bool Moonraker::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    // POST /server/files/upload with `print=true` so Moonraker starts the print
    // itself (issue #14945). It reports back in two fields: print_started when it
    // began the print immediately, print_queued when it could not start it (Klippy
    // not ready, or another print running) but put the job in its queue
    // ([file_manager] queue_gcode_uploads). That covers the power-on case: a
    // [power <device>] section with on_when_job_queued switches the printer on, and
    // with [job_queue] load_on_startup the queue runs the job once Klippy is READY.
    // Either flag means Moonraker owns the print and we must NOT issue our own
    // /printer/print/start. When both are false (no job queue, a non-G-code upload,
    // or a server that ignores `print`) we fall back to the explicit start below.
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    const char *name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    //ORCA: upload_data.storage is plumbed from the (future) per-printer storage dropdown. When unset,
    //      fall back to the Moonraker-standard "gcodes" root. Reading it through here means a UI
    //      addition later (storage picker) needs no change to this method.
    const std::string root = upload_data.storage.empty() ? std::string("gcodes") : upload_data.storage;

    const bool want_start = upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    // The U1 must receive its mapping and calibration in the same command that
    // starts the file. Never let the generic upload start it ahead of that step.
    SnapmakerPrintTask snapmaker_task;
    if (m_snapmaker_u1 && want_start) {
        try {
            if (root != "gcodes")
                throw std::invalid_argument(L("Choose the printer's G-code storage to start this U1 print."));
            if (upload_data.use_3mf)
                throw std::invalid_argument(L("Send G-code to use U1 toolhead assignments and flow calibration."));
            snapmaker_task = decode_snapmaker_print_task(upload_data.extended("snapmaker_u1_task"));
            snapmaker_start_command(upload_filename.string(), snapmaker_task);
        } catch (const nlohmann::json::exception&) {
            error_fn(_L("The U1 print setup is incomplete. Reopen the send dialog and review its assignments."));
            return false;
        } catch (const std::exception& e) {
            error_fn(GUI::format_wxstr(_L("Could not prepare this U1 print: %1%"), _L(e.what())));
            return false;
        }
    }

    std::string url = make_url("server/files/upload");
    bool result = true;
    std::string uploaded_path;
    bool moonraker_started_print = false;
    size_t uploaded_bytes = 0, total_upload_bytes = 0;

    //ORCA: gcode inside a .gcode.3mf is index-coded (Metadata/plate_<N>.gcode), so the upload names the
    //      plate via a 1-based `plateindex` (set only in the .3mf path, see Plater::send_gcode_legacy);
    //      servers that don't use it ignore the unknown form field.
    const std::string plateindex = upload_data.extended("plateindex");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% to %3% (root=%4%, filename=%5%, plateindex=%6%, start_print=%7%)")
        % name
        % upload_data.source_path
        % url
        % root
        % upload_filename.string()
        % (plateindex.empty() ? "-" : plateindex)
        % (want_start ? "true" : "false");

    auto http = Http::post(std::move(url));
    set_auth(http);
    http.form_add("root", root);
    if (!plateindex.empty())
        http.form_add("plateindex", plateindex);
    http.form_add("print", want_start && !m_snapmaker_u1 ? "true" : "false");
    http.form_add_file("file", upload_data.source_path.string(), upload_filename.string())
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: upload HTTP %2%: %3%") % name % status % body;
            if (m_snapmaker_u1 && want_start) {
                try { uploaded_path = snapmaker_uploaded_path(body); }
                catch (const std::exception&) {
                    result = false;
                    error_fn(_L("The U1 did not confirm the uploaded file without starting it. Check the printer before trying again."));
                }
                return;
            }
            try {
                std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                //ORCA: unlike the other endpoints (compare result.klippy_state in test()), the upload
                //      answers with a bare object, not a {"result": ...} envelope: Moonraker's
                //      FileUploadHandler writes finalize_upload()'s dict straight to the response, so
                //      item, print_started and print_queued are top-level. Reading them under result.
                //      silently took the fallbacks below on every upload.
                //ORCA: Moonraker confirms the storage-relative path in item.path. We pass exactly
                //      that string to /printer/print/start so any server-side renaming (.ufp stored as
                //      .gcode, surrounding whitespace and leading slashes stripped) is respected.
                const auto stored_path = ptree.get_optional<std::string>("item.path");
                if (stored_path) {
                    uploaded_path = *stored_path;
                } else {
                    //ORCA: fallback if the server response omits item.path (older Moonraker, or
                    //      a buddy-fork that returns a slimmer envelope). Use the original filename.
                    uploaded_path = upload_filename.string();
                    BOOST_LOG_TRIVIAL(warning) << boost::format(
                        "%1%: upload response missing item.path, falling back to original filename `%2%`")
                        % name % uploaded_path;
                }
                moonraker_started_print = ptree.get<bool>("print_started", false) ||
                                          ptree.get<bool>("print_queued", false);
            } catch (const std::exception &ex) {
                BOOST_LOG_TRIVIAL(warning) << boost::format(
                    "%1%: could not parse upload response (%2%); falling back to original filename")
                    % name % ex.what();
                uploaded_path = upload_filename.string();
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading to %2%: %3%, HTTP %4%, body: `%5%`")
                % name % url % error % status % body;
            error_fn(format_error(body, error, status));
            result = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            uploaded_bytes = progress.ulnow;
            total_upload_bytes = progress.ultotal;
            progress_fn(std::move(progress), cancel);
            if (cancel) {
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                result = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();

    if (!result)
        return false;

    if (m_snapmaker_u1 && want_start) {
        if (moonraker_started_print || uploaded_path.empty()) {
            error_fn(_L("The U1 did not confirm an upload without starting it. Check the printer before trying again."));
            return false;
        }
        wxString error;
        const auto cancelled = [&] {
            const std::string buffer;
            bool cancel = false;
            progress_fn(Http::Progress(0, 0, total_upload_bytes, uploaded_bytes, buffer), cancel);
            return cancel;
        };
        if (!start_snapmaker_print(uploaded_path, snapmaker_task, cancelled, error)) {
            error_fn(std::move(error));
            return false;
        }
        return true;
    }

    // Fallback only when Moonraker neither started nor queued the print. If the
    // printer cannot start it, the error from this request is what the user sees.
    if (want_start && !uploaded_path.empty() && !moonraker_started_print) {
        wxString start_msg;
        if (!start_print(start_msg, uploaded_path)) {
            error_fn(std::move(start_msg));
            return false;
        }
    }
    return true;
}

bool Moonraker::fetch_snapmaker_state(SnapmakerPrinterState& state, wxString& error) const
{
    if (!m_snapmaker_u1) return false;
    auto read = [&](const std::string& path, std::string& body) {
        bool success = false;
        auto http = Http::get(make_url(path));
        set_auth(http);
        http.timeout_connect(5).timeout_max(10)
            .on_complete([&](std::string response, unsigned) { body = std::move(response); success = true; })
            .on_error([&](std::string response, std::string message, unsigned status) {
                error = format_error(response, message, status);
            })
#ifdef WIN32
            .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
            .perform_sync();
        return success;
    };
    std::string status, commands;
    if (!read("printer/objects/query?print_task_config&print_stats&extruder&extruder1&extruder2&extruder3", status) ||
        !read("printer/gcode/help", commands)) return false;
    if (!parse_snapmaker_printer_state(status, commands, state)) {
        error = _L("The printer did not report the U1 toolhead and print-setup information needed for this job.");
        return false;
    }
    return true;
}

bool Moonraker::start_snapmaker_print(const std::string& filename, const SnapmakerPrintTask& task,
                                    const std::function<bool()>& cancelled, wxString& error) const
{
    SnapmakerPrinterState state;
    std::string reason;
    if (!fetch_snapmaker_state(state, error)) return false;
    if (!validate_snapmaker_print_task(task, state, reason)) {
        error = _L(reason.c_str());
        return false;
    }
    std::string command;
    try { command = snapmaker_start_command(filename, task); }
    catch (const std::exception& e) { error = _L(e.what()); return false; }
    if (cancelled()) { error = _L("Print start canceled."); return false; }
    bool success = false;
    auto http = Http::post(make_url("printer/gcode/script"));
    set_auth(http);
    http.header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"script", command}}.dump())
        .timeout_connect(5).timeout_max(30)
        .on_complete([&](std::string body, unsigned) {
            const auto reply = nlohmann::json::parse(body, nullptr, false);
            success = reply.is_object() && !reply.contains("error") && reply.contains("result") &&
                reply["result"].is_string() && reply["result"] == "ok";
            if (!success) error = _L("The U1 did not confirm that this print started. Check the printer before trying again.");
        })
        .on_error([&](std::string body, std::string message, unsigned status) {
            error = _L("The file was uploaded, but the print start was not confirmed. Check the printer before trying again.") +
                "\n" + format_error(body, message, status);
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    // A lost response may follow an accepted start. Do not retry or fall back to
    // /printer/print/start, which would lose the U1 parameters or start twice.
    return success;
}

}
