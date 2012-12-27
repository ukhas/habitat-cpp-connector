/* Copyright 2011-2012 (C) Daniel Richman. License: GNU GPL 3; see LICENSE. */

#ifndef HABITAT_UPLOADER_H
#define HABITAT_UPLOADER_H

#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include "jsoncpp.h"
#include "habitat/EZ.h"
#include "habitat/CouchDB.h"

using namespace std;

namespace habitat {

class UnmergeableError : public runtime_error
{
public:
    UnmergeableError() : runtime_error("habitat::UnmergeableError") {};
    UnmergeableError(const string &what) : runtime_error(what) {};
};

class Uploader
{
    EZ::Mutex mutex;
    const string callsign;
    CouchDB::Server server;
    CouchDB::Database database;
    const int max_merge_attempts;
    string latest_listener_information;
    string latest_listener_telemetry;

    string listener_doc(const char *type, const Json::Value &data,
                        long long int time_created);

public:
    Uploader(const string &callsign,
             const string &couch_uri="http://habitat.habhub.org",
             const string &couch_db="habitat",
             int max_merge_attempts=20);
    ~Uploader() {};
    string payload_telemetry(const string &data,
                             const Json::Value &metadata=Json::Value::null,
                             long long int time_created=-1);
    /* note that latitude, longitude are required properties of data */
    string listener_telemetry(const Json::Value &data,
                              long long int time_created=-1);
    string listener_information(const Json::Value &data,
                                long long int time_created=-1);
    vector<Json::Value> *flights();
    vector<Json::Value> *payloads();
};

} /* namespace habitat */

#endif /* HABITAT_UPLOADER_H */
