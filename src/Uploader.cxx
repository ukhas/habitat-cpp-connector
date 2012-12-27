/* Copyright 2011-2012 (C) Daniel Richman. License: GNU GPL 3; see LICENSE. */

#include "habitat/Uploader.h"
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include "habitat/CouchDB.h"
#include "habitat/EZ.h"
#include "habitat/RFC3339.h"

using namespace std;

namespace habitat {

Uploader::Uploader(const string &callsign, const string &couch_uri,
                   const string &couch_db, int max_merge_attempts)
    : callsign(callsign), server(couch_uri), database(server, couch_db),
      max_merge_attempts(max_merge_attempts)
{
    if (!callsign.length())
        throw invalid_argument("Callsign of zero length");
}

static char hexchar(int n)
{
    if (n < 10)
        return '0' + n;
    else
        return 'a' + n - 10;
}

static string sha256hex(const string &data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    string hexhash;
    hexhash.reserve(SHA256_DIGEST_LENGTH * 2);

    const unsigned char *dc =
        reinterpret_cast<const unsigned char *>(data.c_str());
    SHA256(dc, data.length(), hash);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
        char tmp[2];
        tmp[0] = hexchar((hash[i] & 0xF0) >> 4);
        tmp[1] = hexchar(hash[i] & 0x0F);
        hexhash.append(tmp, 2);
    }

    return hexhash;
}

static string base64(const string &data)
{
    /* So it's either this or linking with another b64 library... */
    BIO *bio_mem, *bio_b64;

    bio_b64 = BIO_new(BIO_f_base64());
    bio_mem = BIO_new(BIO_s_mem());
    if (bio_b64 == NULL || bio_mem == NULL)
        throw runtime_error("Base64 conversion failed");

    BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);

    bio_b64 = BIO_push(bio_b64, bio_mem);
    /* Chain is now ->b64->mem */

    size_t result_a;
    int result_b;

    result_a = BIO_write(bio_b64, data.c_str(), data.length());
    result_b = BIO_flush(bio_b64);

    if (result_a != data.length() || result_b != 1)
        throw runtime_error("Base64 conversion failed: BIO_write,flush");

    char *data_b64_c;
    size_t data_b64_length;

    data_b64_length = BIO_get_mem_data(bio_mem, &data_b64_c);
    string data_b64(data_b64_c, data_b64_length);

    BIO_free_all(bio_b64);

    return data_b64;
}

static void set_time(Json::Value &thing, long long int time_created)
{
    thing["time_uploaded"] = RFC3339::now_to_rfc3339_localoffset();
    thing["time_created"] =
        RFC3339::timestamp_to_rfc3339_localoffset(time_created);
}

string Uploader::payload_telemetry(const string &data,
                                   const Json::Value &metadata,
                                   long long int time_created)
{
    EZ::MutexLock lock(mutex);

    if (!data.length())
        throw runtime_error("Can't upload string of zero length");

    string data_b64 = base64(data);
    string doc_id = sha256hex(data_b64);

    if (time_created == -1)
        time_created = time(NULL);

    Json::Value doc;
    doc["data"] = Json::Value(Json::objectValue);
    doc["data"]["_raw"] = data_b64;
    doc["receivers"] = Json::Value(Json::objectValue);
    doc["receivers"][callsign] = Json::Value(Json::objectValue);

    Json::Value &receiver_info = doc["receivers"][callsign];

    if (metadata.isObject())
    {
        if (metadata.isMember("time_created") ||
            metadata.isMember("time_uploaded") ||
            metadata.isMember("latest_listener_information") ||
            metadata.isMember("latest_listener_telemetry"))
        {
            throw invalid_argument("found forbidden key in metadata");
        }

        /* This copies metadata. */
        receiver_info = metadata;
    }
    else if (!metadata.isNull())
    {
        throw invalid_argument("metadata must be an object/dict or null");
    }

    if (latest_listener_information.length())
        receiver_info["latest_listener_information"] =
            latest_listener_information;

    if (latest_listener_telemetry.length())
        receiver_info["latest_listener_telemetry"] = latest_listener_telemetry;

    for (int attempts = 0; attempts < max_merge_attempts; attempts++)
    {
        try
        {
            set_time(receiver_info, time_created);
            database.update_put("payload_telemetry", "add_listener", doc_id,
                                doc);
            return doc_id;
        }
        catch (CouchDB::Conflict &e)
        {
            continue;
        }
        catch (EZ::HTTPResponse &e)
        {
            if (e.response_code == 403 || e.response_code == 401)
                break; // Unmergeable
            else
                throw;
        }
    }

    throw UnmergeableError();
}

string Uploader::listener_doc(const char *type, const Json::Value &data,
                              long long int time_created)
{
    if (time_created == -1)
        time_created = time(NULL);

    if (!data.isObject())
        throw invalid_argument("data must be an object/dict");

    if (data.isMember("callsign"))
        throw invalid_argument("forbidden key in data");

    Json::Value copied_data(data);
    copied_data["callsign"] = callsign;

    Json::Value doc(Json::objectValue);
    doc["data"] = copied_data;
    doc["type"] = type;

    set_time(doc, time_created);
    database.save_doc(doc);

    return doc["_id"].asString();
}

string Uploader::listener_telemetry(const Json::Value &data,
                                    long long int time_created)
{
    EZ::MutexLock lock(mutex);

    latest_listener_telemetry =
        listener_doc("listener_telemetry", data, time_created);
    return latest_listener_telemetry;
}

string Uploader::listener_information(const Json::Value &data,
                                      long long int time_created)
{
    EZ::MutexLock lock(mutex);

    latest_listener_information =
        listener_doc("listener_information", data, time_created);
    return latest_listener_information;
}

vector<Json::Value> *Uploader::flights()
{
    map<string,string> options;

    Json::Value startkey(Json::arrayValue);
#ifdef JSON_HAS_INT64
    startkey.append((Json::Int64) time(NULL));
#else
    startkey.append((Json::Int) time(NULL));
#endif

    options["include_docs"] = "true";
    options["startkey"] = CouchDB::Database::json_query_value(startkey);

    Json::Value *response =
        database.view("flight", "end_start_including_payloads", options);
    auto_ptr<Json::Value> response_destroyer(response);

    vector<Json::Value> *result = new vector<Json::Value>;
    auto_ptr< vector<Json::Value> > result_destroyer(result);

    if (!response->isObject())
        throw runtime_error("Invalid response: was not an object");

    const Json::Value &rows = (*response)["rows"];
    Json::Value::const_iterator it;

    if (!rows.isArray())
        throw runtime_error("Invalid response: rows was not an array");

    result->reserve(rows.size());
    Json::Value *current_pcfg_list = NULL;

    for (it = rows.begin(); it != rows.end(); it++)
    {
        const Json::Value &row = *it;
        if (!row.isObject())
            throw runtime_error("Invalid response: row was not an object");

        const Json::Value &key = row["key"], &doc = row["doc"];

        bool doc_ok = doc.isObject() && doc.size();
        bool key_ok = key.isArray() && key.size() == 4 && key[3u].isIntegral();

        if (!key_ok)
            throw runtime_error("Invalid response: bad key in row");

        bool is_pcfg = key[3u].asBool();

        if (!is_pcfg)
        {
            if (!doc_ok)
                throw runtime_error("Invalid response: bad doc in row");

            result->push_back(doc);
            /* copies the doc */

            Json::Value &doc_copy = result->back();
            doc_copy["_payload_docs"] = Json::Value(Json::arrayValue);
            current_pcfg_list = &(doc_copy["_payload_docs"]);
        }
        else
        {
            if (doc_ok)
                current_pcfg_list->append(doc);
        }
    }

    result_destroyer.release();

    return result;
}

vector<Json::Value> *Uploader::payloads()
{
    map<string,string> options;
    options["include_docs"] = "true";

    Json::Value *response =
        database.view("payload_configuration", "name_time_created", options);
    auto_ptr<Json::Value> response_destroyer(response);

    vector<Json::Value> *result = new vector<Json::Value>;
    auto_ptr< vector<Json::Value> > result_destroyer(result);

    if (!response->isObject())
        throw runtime_error("Invalid response: was not an object");

    const Json::Value &rows = (*response)["rows"];
    Json::Value::const_iterator it;

    if (!rows.isArray())
        throw runtime_error("Invalid response: rows was not an array");

    result->reserve(rows.size());

    for (it = rows.begin(); it != rows.end(); it++)
    {
        if (!(*it).isObject())
            throw runtime_error("Invalid response: doc was not an object");
        result->push_back((*it)["doc"]);
    }

    result_destroyer.release();
    return result;
}

} /* namespace habitat */
