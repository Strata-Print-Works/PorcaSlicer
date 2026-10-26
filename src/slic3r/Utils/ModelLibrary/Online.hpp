#pragma once
#include "Library.hpp"
#include <map>

namespace Slic3r::ModelLibrary {
struct WebReply { unsigned status = 0; std::string body; };
using WebRequest = std::function<WebReply(const std::string& url, const Json& body,
                                         const std::map<std::string, std::string>& headers, const Cancel& cancel)>;
// The core never opens sockets. Production supplies Porca's HTTP client; tests
// supply recorded replies, including failures and cancellations.
Json ai_request(const Library& library, const std::vector<Id>& ids, const Json& vocabulary);
Json ai_answers(const Json& response, const std::vector<Id>& asked, const Json& vocabulary);
Json page_details(const std::string& url, const WebRequest& web, const Cancel& cancel = {});
void tag_with_ai(Library& library, const std::string& key, const WebRequest& web, const Cancel& cancel = {});
void enrich_pages(Library& library, const WebRequest& web, const Cancel& cancel = {});
WebReply request_online(const std::string& url, const Json& body,
                        const std::map<std::string, std::string>& headers, const Cancel& cancel);
}
