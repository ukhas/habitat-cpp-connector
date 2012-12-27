/* Copyright 2011-2012 (C) Daniel Richman. License: GNU GPL 3; see LICENSE. */

#include "habitat/CouchDB.h"
#include <curl/curl.h>
#include <string>
#include <memory>
#include <stdexcept>
#include "habitat/EZ.h"

using namespace std;

namespace CouchDB {

const map<string,string> Database::view_default_options;

static string server_url(const string &url)
{
    if (!url.length())
        throw invalid_argument("URL of zero length");

    string url_ts(url);

    if (*(url.rbegin()) != '/')
        url_ts.append("/");

    return url_ts;
}

static string database_url(const string &server_url, const string &db)
{
    if (!db.length())
        throw invalid_argument("DB of zero length");

    string url(server_url);
    url.append(EZ::cURL::escape(db));

    if (*(db.rbegin()) != '/')
        url.append("/");

    return url;
}

Server::Server(const string &url)
    : url(server_url(url)) {}

Database::Database(Server &server, const string &db)
    : server(server), url(database_url(server.url, db)) {}

string Server::next_uuid()
{
    EZ::MutexLock lock(uuid_cache_mutex);
    string uuid;

    if (uuid_cache.size())
    {
        uuid = uuid_cache.front();
        uuid_cache.pop_front();
        return uuid;
    }
    else
    {
        string uuid_url(url);
        uuid_url.append("_uuids?count=100");

        Json::Value *root = get_json(uuid_url);
        auto_ptr<Json::Value> value_destroyer(root);

        const Json::Value &uuids = (*root)["uuids"];
        if (!uuids.isArray() || !uuids.size())
            throw runtime_error("Invalid UUIDs response");

        uuid = uuids[Json::UInt(0)].asString();

        for (Json::UInt index = 1; index < uuids.size(); index++)
            uuid_cache.push_back(uuids[index].asString());
    }

    return uuid;
}

Json::Value *Server::get_json(const string &get_url)
{
    Json::Reader reader;
    Json::Value *doc = new Json::Value;
    auto_ptr<Json::Value> value_destroyer(doc);

    string response = curl.get(get_url);

    if (!reader.parse(response, *doc, false))
        throw runtime_error("JSON Parsing error");

    value_destroyer.release();

    return doc;
}

string Database::make_doc_url(const string &doc_id) const
{
    string doc_url(url);
    doc_url.append(EZ::cURL::escape(doc_id));
    return doc_url;
}

Json::Value *Database::operator[](const string &doc_id)
{
    return get_doc(doc_id);
}

void Database::save_doc(Json::Value &doc)
{
    Json::Value &id = doc["_id"];

    if (id.isNull())
        id = server.next_uuid();

    if (!id.isString())
        throw runtime_error("_id must be a string if set");

    string doc_id = id.asString();

    if (doc_id.length() == 0)
        throw runtime_error("_id cannot be an empty string");
    if (doc_id[0] == '_')
        throw runtime_error("_id cannot start with _");

    Json::FastWriter writer;
    string json_doc = writer.write(doc);

    string doc_url = make_doc_url(doc_id);

    string response;

    try
    {
        response = server.curl.put(doc_url, json_doc);
    }
    catch (EZ::HTTPResponse &e)
    {
        /* Catch HTTP 409 Resource Conflict */

        if (e.response_code != 409)
            throw;

        throw Conflict(doc_id);
    }

    Json::Reader reader;
    Json::Value info;

    if (!reader.parse(response, info, false))
        throw runtime_error("JSON Parsing error");

    const Json::Value &new_id = info["id"];
    const Json::Value &new_rev = info["rev"];

    if (!new_id.isString() || !new_rev.isString())
        throw runtime_error("Invalid server response (id, rev !string)");

    if (new_id.asString() != doc_id)
        throw runtime_error("Server has gone insane (saved wrong _id)");

    doc["_rev"] = new_rev;
}

Json::Value *Database::get_doc(const string &doc_id)
{
    return server.get_json(make_doc_url(doc_id));
}

Json::Value *Database::view(const string &design_doc, const string &view_name,
                            const map<string,string> &options)
{
    string view_url(url);

    if (design_doc.length())
    {
        view_url.append("_design/");
        view_url.append(EZ::cURL::escape(design_doc));
        view_url.append("/_view/");
    }

    view_url.append(EZ::cURL::escape(view_name));

    if (options.size())
    {
        view_url.append(EZ::cURL::query_string(options, true));
    }

    return server.get_json(view_url);
}

string Database::update_put(const string &design_doc,
                            const string &update_name,
                            const string &doc_id,
                            const Json::Value &payload)
{
    Json::FastWriter writer;
    string json_payload = writer.write(payload);
    return update_put(design_doc, update_name, doc_id, json_payload);
}

string Database::update_put(const string &design_doc,
                            const string &update_name,
                            const string &doc_id,
                            const string &payload)
{
    string update_url(url);

    update_url.append("_design/");
    update_url.append(EZ::cURL::escape(design_doc));
    update_url.append("/_update/");
    update_url.append(EZ::cURL::escape(update_name));

    if (doc_id.size())
    {
        update_url.append("/");
        update_url.append(doc_id);
    }

    try
    {
        return server.curl.put(update_url, payload);
    }
    catch (EZ::HTTPResponse &e)
    {
        /* Catch HTTP 409 Resource Conflict */

        if (e.response_code != 409)
            throw;

        throw Conflict(doc_id);
    }
}

string Database::json_query_value(Json::Value &value)
{
    Json::FastWriter writer;
    string str = writer.write(value);
    int final = str.length() - 1;
    if (final >= 0 && str[final] == '\n')
        str.erase(final);
    return str;
}

} /* namespace CouchDB */
