/* Copyright 2012 (C) Daniel Richman. License: GNU GPL 3; see LICENSE. */

#include <iostream>
#include <stdexcept>
#include <json/json.h>
#include <ctime>

#include "RFC3339.h"

using namespace std;

void handle_command(const Json::Value &command);

int main(int argc, char **argv)
{
    tzset();

    for (;;)
    {
        char line[1024];
        cin.getline(line, 1024);

        if (line[0] == '\0')
            break;

        Json::Reader reader;
        Json::Value command;

        if (!reader.parse(line, command, false))
            throw runtime_error("JSON parsing failed");

        if (!command.isArray() || !command[0u].isString())
            throw runtime_error("Invalid JSON input");

        handle_command(command);
    }
}

void reply(const Json::Value &arg1, const Json::Value &arg2)
{
    Json::Value response(Json::arrayValue);
    response.append(arg1);
    response.append(arg2);
    Json::FastWriter writer;
    cout << writer.write(response);
}

void handle_command(const Json::Value &command)
{
    string command_name = command[0u].asString();
    string string_arg;
    long long int int_arg = 0;

    if (command_name == "validate_rfc3339" ||
        command_name == "rfc3339_to_timestamp")
    {
        string_arg = command[1u].asString();
    }
    else
    {
#ifdef JSON_HAS_INT64
        int_arg = command[1u].asLargestInt();
#else
        int_arg = command[1u].asInt();
#endif
    }

    try
    {
        if (command_name == "validate_rfc3339")
            reply("return", RFC3339::validate_rfc3339(string_arg));
        else if (command_name == "rfc3339_to_timestamp")
            reply("return", RFC3339::rfc3339_to_timestamp(string_arg));
        else if (command_name == "timestamp_to_rfc3339_utcoffset")
            reply("return", RFC3339::timestamp_to_rfc3339_utcoffset(int_arg));
        else if (command_name == "timestamp_to_rfc3339_localoffset")
            reply("return", RFC3339::timestamp_to_rfc3339_localoffset(int_arg));
        else if (command_name == "now_to_rfc3339_utcoffset")
            reply("return", RFC3339::now_to_rfc3339_utcoffset());
        else if (command_name == "now_to_rfc3339_localoffset")
            reply("return", RFC3339::now_to_rfc3339_localoffset());
        else
            throw runtime_error("Command not found");
    }
    catch (out_of_range &e)
    {
        reply("time_t error", false);
    }
    catch (exception &e)
    {
        reply("exception", e.what());
    }
}
