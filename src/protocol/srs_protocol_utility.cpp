// srs_simple — 원본: trunk/src/protocol/srs_protocol_utility.cpp (+ kernel/srs_kernel_utility.cpp의 문자열 헬퍼)
#include <srs_protocol_utility.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <srs_protocol_rtmp_stack.hpp>

using std::string;

// query string("k1=v1&k2=v2")에서 key의 값을 찾는다. 없으면 빈 문자열.
// (원본은 SrsHttpUri::get_query_by_key — HTTP 스택이 없으므로 파일-로컬로 축소)
static string srs_query_get_by_key(const string& query, const string& key)
{
    size_t pos = 0;
    while (pos < query.length()) {
        size_t next = query.find('&', pos);
        string kv = query.substr(pos, next == string::npos ? string::npos : next - pos);

        size_t eq = kv.find('=');
        if (eq != string::npos && kv.substr(0, eq) == key) {
            return kv.substr(eq + 1);
        }

        if (next == string::npos) {
            break;
        }
        pos = next + 1;
    }
    return "";
}

void srs_discovery_tc_url(string tcUrl, string& schema, string& host, string& vhost,
    string& app, string& stream, int& port, string& param)
{
    // For compatibility, transform
    //      rtmp://ip/app...vhost...VHOST/stream
    // to typical format:
    //      rtmp://ip/app?vhost=VHOST/stream
    string fullUrl = srs_string_replace(tcUrl, "...vhost...", "?vhost=");

    // Standard URL is:
    //      rtmp://ip/app/app2/stream?k=v
    // Where after last slash is stream.
    fullUrl += stream.empty() ? "/" : (stream.at(0) == '/' ? stream : "/" + stream);
    fullUrl += param.empty() ? "" : (param.at(0) == '?' ? param : "?" + param);

    // First, we covert the FMLE URL to standard URL:
    //      rtmp://ip/app/app2?k=v/stream , or:
    //      rtmp://ip/app/app2#k=v/stream
    size_t pos_query = fullUrl.find_first_of("?#");
    size_t pos_rslash = fullUrl.rfind("/");
    if (pos_rslash != string::npos && pos_query != string::npos && pos_query < pos_rslash) {
        fullUrl = fullUrl.substr(0, pos_query) // rtmp://ip/app/app2
                  + fullUrl.substr(pos_rslash) // /stream
                  + fullUrl.substr(pos_query, pos_rslash - pos_query); // ?k=v
    }

    // Remove the _definst_ of FMLE URL.
    if (fullUrl.find("/_definst_") != string::npos) {
        fullUrl = srs_string_replace(fullUrl, "/_definst_", "");
    }

    // Parse the standard URL: schema://host[:port]/path[?query]
    // (원본은 SrsHttpUri로 파싱 — HTTP 스택이 없으므로 손 파싱)
    string rest = fullUrl;
    size_t pos = rest.find("://");
    if (pos != string::npos) {
        schema = rest.substr(0, pos);
        rest = rest.substr(pos + 3);
    } else {
        schema = "rtmp";
    }

    string hostport = rest;
    string path;
    if ((pos = rest.find("/")) != string::npos) {
        hostport = rest.substr(0, pos);
        path = rest.substr(pos + 1);
    }

    host = hostport;
    port = SRS_CONSTS_RTMP_DEFAULT_PORT;
    if ((pos = hostport.find(":")) != string::npos) {
        host = hostport.substr(0, pos);
        port = ::atoi(hostport.substr(pos + 1).c_str());
        if (port <= 0) {
            port = SRS_CONSTS_RTMP_DEFAULT_PORT;
        }
    }

    string query;
    if ((pos = path.find_first_of("?#")) != string::npos) {
        query = path.substr(pos + 1);
        path = path.substr(0, pos);
    }

    // After last slash of path is stream, before is app.
    if ((pos = path.rfind("/")) != string::npos) {
        stream = path.substr(pos + 1);
        app = path.substr(0, pos);
    } else {
        app = path;
        stream = "";
    }
    if (app.empty()) {
        app = SRS_CONSTS_RTMP_DEFAULT_APP;
    }

    param = query.empty() ? "" : "?" + query;

    // Try to parse vhost from query, or use host if not specified.
    string vhost_in_query = srs_query_get_by_key(query, "vhost");
    if (vhost_in_query.empty()) {
        vhost_in_query = srs_query_get_by_key(query, "domain");
    }
    if (!vhost_in_query.empty() && vhost_in_query != SRS_CONSTS_RTMP_DEFAULT_VHOST) {
        vhost = vhost_in_query;
    }
    if (vhost.empty()) {
        vhost = host;
    }

    // Only one param, the default vhost, clear it.
    if (param.find("&") == string::npos && vhost_in_query == SRS_CONSTS_RTMP_DEFAULT_VHOST) {
        param = "";
    }
}

string srs_generate_stream_url(string vhost, string app, string stream)
{
    std::string url = "";

    if (SRS_CONSTS_RTMP_DEFAULT_VHOST != vhost) {
        url += vhost;
    }
    url += "/" + app;
    url += "/" + stream;

    return url;
}

string srs_path_build_stream(string template_path, string vhost, string app, string stream)
{
    std::string path = template_path;

    // variable [vhost]
    path = srs_string_replace(path, "[vhost]", vhost);
    // variable [app]
    path = srs_string_replace(path, "[app]", app);
    // variable [stream]
    path = srs_string_replace(path, "[stream]", stream);

    return path;
}

string srs_get_peer_ip(int fd)
{
    // 원본은 getnameinfo로 IPv6까지 처리하지만, IPv4 전용으로 축소 (S2의 srs_tcp_listen과 동일).
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (::getpeername(fd, (sockaddr*)&addr, &addrlen) == -1) {
        return "";
    }

    char buf[INET_ADDRSTRLEN];
    if (::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == NULL) {
        return "";
    }

    return buf;
}

int srs_get_peer_port(int fd)
{
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (::getpeername(fd, (sockaddr*)&addr, &addrlen) == -1) {
        return 0;
    }

    return ntohs(addr.sin_port);
}

string srs_get_local_ip(int fd)
{
    sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    if (::getsockname(fd, (sockaddr*)&addr, &addrlen) == -1) {
        return "";
    }

    char buf[INET_ADDRSTRLEN];
    if (::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == NULL) {
        return "";
    }

    return buf;
}

string srs_string_replace(string str, string old_str, string new_str)
{
    std::string ret = str;

    if (old_str == new_str) {
        return ret;
    }

    size_t pos = 0;
    while ((pos = ret.find(old_str, pos)) != std::string::npos) {
        ret = ret.replace(pos, old_str.length(), new_str);
        pos += new_str.length();
    }

    return ret;
}

string srs_string_trim_end(string str, string trim_chars)
{
    std::string ret = str;

    for (int i = 0; i < (int)trim_chars.length(); i++) {
        char ch = trim_chars.at(i);

        while (!ret.empty() && ret.at(ret.length() - 1) == ch) {
            ret.erase(ret.end() - 1);

            // ok, matched, should reset the search
            i = -1;
            break;
        }
    }

    return ret;
}

string srs_string_trim_start(string str, string trim_chars)
{
    std::string ret = str;

    for (int i = 0; i < (int)trim_chars.length(); i++) {
        char ch = trim_chars.at(i);

        while (!ret.empty() && ret.at(0) == ch) {
            ret.erase(ret.begin());

            // ok, matched, should reset the search
            i = -1;
            break;
        }
    }

    return ret;
}

string srs_string_remove(string str, string remove_chars)
{
    std::string ret = str;

    for (int i = 0; i < (int)remove_chars.length(); i++) {
        char ch = remove_chars.at(i);

        for (std::string::iterator it = ret.begin(); it != ret.end();) {
            if (ch == *it) {
                it = ret.erase(it);

                // ok, matched, should reset the search
                i = -1;
            } else {
                ++it;
            }
        }
    }

    return ret;
}
