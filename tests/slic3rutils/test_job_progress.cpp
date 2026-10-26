#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/ProgressIndicator.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <wx/string.h>

namespace {

class RecordedProgress final : public Slic3r::ProgressIndicator
{
public:
    std::vector<std::string> events;
    std::vector<std::thread::id> threads;
    int error_code = 0;
    wxString description;
    wxString extra;

    void record(std::string event)
    {
        events.push_back(std::move(event));
        threads.push_back(std::this_thread::get_id());
    }
    void clear_percent() override { record("clear"); }
    void show_error_info(wxString msg, int code, wxString desc, wxString info) override
    {
        error_code = code;
        description = desc;
        extra = info;
        record(msg.ToStdString());
    }
    void set_range(int) override {}
    int get_range() const override { return 100; }
    void set_cancel_callback(CancelFn) override {}
    void set_progress(int) override {}
    void set_status_text(const char* msg) override { record(msg); }
};

class FailedTransfer final : public Slic3r::GUI::Job
{
    std::shared_ptr<RecordedProgress> m_progress;

public:
    std::promise<void> reported;
    explicit FailedTransfer(std::shared_ptr<RecordedProgress> progress) : m_progress(std::move(progress)) {}
    void process(Ctl& ctl) override
    {
        ctl.update_status(40, "sending");
        ctl.clear_percent();
        ctl.show_error_info("failed", -42, "Upload failed", "Storage unavailable");
        reported.set_value();
    }
    void finalize(bool, std::exception_ptr&) override { m_progress->record("finished"); }
};

} // namespace

TEST_CASE("Transfer errors reach the UI thread after queued progress", "[JobProgress]")
{
    const auto main_thread = std::this_thread::get_id();
    auto progress = std::make_shared<RecordedProgress>();
    Slic3r::GUI::BoostThreadWorker worker(progress, "offline-progress");
    auto job = std::make_shared<FailedTransfer>(progress);
    auto reported = job->reported.get_future();
    REQUIRE(worker.push(job));
    REQUIRE(reported.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    // Nothing may touch the indicator before the main thread drains the queue.
    CHECK(progress->events.empty());
    REQUIRE(worker.wait_for_idle(5000));
    CHECK(progress->events == std::vector<std::string>{"sending", "clear", "failed", "finished"});
    for (const auto& thread : progress->threads)
        CHECK(thread == main_thread);
    CHECK(progress->error_code == -42);
    CHECK(progress->description == "Upload failed");
    CHECK(progress->extra == "Storage unavailable");
}

TEST_CASE("Waiting for short jobs drains every finalization", "[JobProgress]")
{
    unsigned finished = 0;
    Slic3r::GUI::BoostThreadWorker worker(nullptr, "offline-completion");
    for (unsigned count = 1; count <= 512; ++count) {
        REQUIRE(Slic3r::GUI::queue_job(worker, [](Slic3r::GUI::Job::Ctl&) {},
            [&](bool, std::exception_ptr&) { ++finished; }));
        REQUIRE(worker.wait_for_idle(5000));
        REQUIRE(finished == count);
        REQUIRE(worker.is_idle());
    }
}
