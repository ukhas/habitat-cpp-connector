/* Copyright 2011 (C) Daniel Richman. License: GNU GPL 3; see COPYING. */

#include "habitat/UKHASExtractor.h"
#include <stdexcept>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <stdint.h>
#include "jsoncpp.h"

using namespace std;

namespace habitat {

void UKHASExtractor::reset_buffer()
{
    buffer.resize(0);
    buffer.clear();
    buffer.reserve(256);
}

void UKHASExtractor::skipped(int n)
{
    if (extracting)
    {
        skipped_count += n;

        /* If radio goes silent for too long (~10s at 50baud,
         * ~1.5s at 300baud) */
        if (skipped_count > 50)
        {
            mgr->status("UKHAS Extractor: giving up (silence)");
            reset_buffer();
            extracting = false;
        }
    }
}

void UKHASExtractor::push(char b, enum push_flags flags)
{
    if (b == '\r') b = '\n';

    if (last == '$' && b == '$')
    {
        /* Start delimiter: "$$" */
        reset_buffer();
        buffer.push_back(last);
        buffer.push_back(b);

        garbage_count = 0;
        skipped_count = 0;
        extracting = true;

        mgr->status("UKHAS Extractor: found start delimiter");
    }
    else if (extracting && b == '\n')
    {
        /* End delimiter: "\n" */
        buffer.push_back(b);
        mgr->uthr.payload_telemetry(buffer);

        mgr->status("UKHAS Extractor: extracted string");

        try
        {
            mgr->data(crude_parse());
        }
        catch (runtime_error &e)
        {
            mgr->status("UKHAS Extractor: crude parse failed: " +
                        string(e.what()));

            Json::Value bare(Json::objectValue);
            bare["_sentence"] = buffer;
            mgr->data(bare);
        }

        reset_buffer();
        extracting = false;
    }
    else if (extracting)
    {
        /* baudot doesn't support '*', so we use '#'. */
        if ((flags & PUSH_BAUDOT_HACK) && b == '#')
            b = '*';

        buffer.push_back(b);

        if (b < 0x20 || b > 0x7E)
            garbage_count++;

        /* Sane limits to avoid uploading tonnes of garbage */
        if (buffer.length() > 1000 || garbage_count > 32)
        {
            mgr->status("UKHAS Extractor: giving up");

            reset_buffer();
            extracting = false;
        }
    }

    last = b;
}

static void inplace_toupper(char &c)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;
}

static string checksum_xor(const string &s)
{
    uint8_t checksum = 0;
    for (string::const_iterator it = s.begin(); it != s.end(); it++)
        checksum ^= (*it);

    char temp[3];
    snprintf(temp, sizeof(temp), "%.02X", checksum);
    return string(temp);
}

static string checksum_crc16_ccitt(const string &s)
{
    /* From avr-libc docs: Modified BSD (GPL, BSD, DFSG compatible) */
    uint16_t crc = 0xFFFF;

    for (string::const_iterator it = s.begin(); it != s.end(); it++)
    {
        crc = crc ^ ((uint16_t (*it)) << 8);

        for (int i = 0; i < 8; i++)
        {
            bool s = crc & 0x8000;
            crc <<= 1;
            crc ^= (s ? 0x1021 : 0);
        }
    }

    char temp[5];
    snprintf(temp, sizeof(temp), "%.04X", crc);
    return string(temp);
}

static vector<string> split(const string &input, const char c)
{
    vector<string> parts;
    size_t pos = 0, lastpos = 0;

    do
    {
        /* pos returns npos? substr will grab to end of string. */
        pos = input.find_first_of(c, lastpos);

        if (pos == string::npos)
            parts.push_back(input.substr(lastpos));
        else
            parts.push_back(input.substr(lastpos, pos - lastpos));

        lastpos = pos + 1;
    }
    while (pos != string::npos);

    return parts;
}

static void split_string(const string &buffer, string *data, string *checksum)
{
    if (buffer.substr(0, 2) != "$$")
        throw runtime_error("String does not begin with $$");

    if (buffer[buffer.length() - 1] != '\n')
        throw runtime_error("String does not end with '\\n'");

    size_t pos = buffer.find_last_of('*');
    if (pos == string::npos)
        throw runtime_error("No checksum");

    size_t check_start = pos + 1;
    size_t check_end = buffer.length() - 1;
    size_t check_length = check_end - check_start;

    if (check_length != 2 && check_length != 4)
        throw runtime_error("Invalid checksum length");

    size_t data_start = 2;
    size_t data_length = pos - data_start;

    *data = buffer.substr(data_start, data_length);
    *checksum = buffer.substr(check_start, check_length);
}

static string examine_checksum(const string &data, const string &checksum_o)
{
    string checksum = checksum_o;
    for_each(checksum.begin(), checksum.end(), inplace_toupper);

    string expect, name;

    if (checksum.length() == 2)
    {
        expect = checksum_xor(data);
        name = "xor";
    }
    else if (checksum.length() == 4)
    {
        expect = checksum_crc16_ccitt(data);
        name = "crc16-ccitt";
    }
    else
    {
        throw runtime_error("Invalid checksum length");
    }

    if (expect != checksum)
        throw runtime_error("Invalid checksum: expected " + expect);

    return name;
}

static bool is_ddmmmm_field(const Json::Value &field)
{
    if (field["sensor"] != "stdtelem.coordinate")
        return false;

    if (!field["format"].isString())
        return false;

    string format = field["format"].asString();

    /* does it match d+m+\.m+ ? */

    size_t pos;

    pos = format.find_first_not_of('d');
    if (pos == string::npos || format[pos] != 'm')
        return false;

    pos = format.find_first_not_of('m', pos);
    if (pos == string::npos || format[pos] != '.')
        return false;

    pos++;

    pos = format.find_first_not_of('m', pos);
    if (pos != string::npos)
        return false;

    return true;
}

static string convert_ddmmmm(const string &value)
{
    size_t split = value.find('.');
    if (split == string::npos || split <= 2)
        throw runtime_error("invalid '.' pos when converting ddmm");

    string left = value.substr(0, split - 2);
    string right = value.substr(split - 2);

    istringstream lis(left), ris(right);
    double left_val, right_val;
    lis >> left_val;
    ris >> right_val;

    if (lis.fail() || ris.fail() || lis.peek() != EOF || ris.peek() != EOF)
        throw runtime_error("couldn't parse left or right parts (ddmm)");

    if (right_val >= 60 || right_val < 0)
        throw runtime_error("invalid right part (ddmm)");

    double dd = left_val + (right_val / 60);
    
    ostringstream os;
    os.precision(value.length() - value.find_first_not_of("0+-") - 2);
    os << dd;
    return os.str();
}

static bool is_numeric_field(const Json::Value &field)
{
    return field["sensor"] == "base.ascii_int" ||
            field["sensor"] == "base.ascii_float";
}

static double convert_numeric(const string &value)
{
    istringstream is(value);
    double val;
    is >> val;
    if (is.fail())
        throw runtime_error("couldn't parse numeric value");
    return val;
}

static void extract_fields(Json::Value &data, const Json::Value &fields,
                           const vector<string> &parts)
{
    vector<string>::const_iterator part = parts.begin() + 1;
    Json::Value::const_iterator field = fields.begin();

    while (field != fields.end() && part != parts.end())
    {
        if (!(*field).isObject())
            throw runtime_error("Invalid configuration (field not an object)");

        const string key = (*field)["name"].asString();
        const string value = (*part);

        if (!key.length())
            throw runtime_error("Invalid configuration (empty field name)");

        if (value.length())
        {
            if (is_ddmmmm_field(*field))
                data[key] = convert_ddmmmm(value);
            else if (is_numeric_field(*field))
                data[key] = convert_numeric(value);
            else
                data[key] = value;
        }

        field++;
        part++;
    }
}

static void numeric_scale(Json::Value &data, const Json::Value &config)
{
    const string source = config["source"].asString();
    string destination = source;

    if (!config["destination"].isNull())
    {
        if (!config["destination"].isString())
            throw runtime_error("Invalid (numeric scale) configuration "
                                "(non string destination)");
        destination = config["destination"].asString();
    }

    if (destination == "payload" ||
            (destination.size() && destination[0] == '_'))
        throw runtime_error("Invalid (numeric scale) configuration "
                            "(forbidden destination)");

    if (!data[source].isNumeric())
        throw runtime_error("Attempted to apply numeric scale to "
                            "(non numeric source value)");
    if (!config["factor"].isNumeric())
        throw runtime_error("Invalid (numeric scale) configuration "
                            "(non numeric factor)");
    if (!config["source"].isString())
        throw runtime_error("Invalid (numeric scale) configuration "
                            "(non string source)");

    double value = data[source].asDouble();
    double factor = config["factor"].asDouble();

    value *= factor;

    if (!config["offset"].isNull())
    {
        if (!config["offset"].isNumeric())
            throw runtime_error("Invalid (numeric scale) configuration "
                                "(non numeric offset)");

        double offset = config["offset"].asDouble();

        value += offset;
    }

    if (!config["round"].isNull())
    {
        if (!config["round"].isNumeric())
            throw runtime_error("Invalid (numeric scale) configuration "
                                "(non numeric round)");

        double round_d = config["round"].asDouble();
        int round_i = int(round_d);

        if (fabs(double(round_i) - round_d) > 0.001)
            throw runtime_error("Invalid (numeric scale) configuration "
                                "(non integral round)");

        if (value != 0)
        {
            int position = round_i - int(ceil(log10(fabs(value))));
            double m = pow(10.0, position);
            value = round(value * m) / m;
        }
    }

    data[destination] = value;
}

static void post_filters(Json::Value &data, const Json::Value &sentence)
{
    if (!sentence["filters"].isObject())
        return;

    const Json::Value post_filters = sentence["filters"]["post"];

    if (!post_filters.isArray())
        return;

    for (Json::Value::const_iterator it = post_filters.begin();
         it != post_filters.end(); it++)
    {
        if ((*it)["type"] == "normal" &&
            (*it)["filter"] == "common.numeric_scale")
            numeric_scale(data, *it);
    }
}

static void cook_basic(Json::Value &basic, const string &buffer,
                       const string &callsign)
{
    basic["_sentence"] = buffer;
    basic["_protocol"] = "UKHAS";
    basic["_parsed"] = true;
    basic["payload"] = callsign;
}

static void attempt_settings(Json::Value &data, const Json::Value &sentence,
                             const string &checksum_name,
                             const vector<string> &parts)
{
    if (!sentence.isObject() || !sentence["callsign"].isString() ||
        !sentence["fields"].isArray() || !sentence["fields"].size())
        throw runtime_error("Invalid configuration "
                                "(missing callsign or fields)");

    if (sentence["callsign"] != parts[0])
        throw runtime_error("Incorrect callsign");

    if (sentence["checksum"] != checksum_name)
        throw runtime_error("Wrong checksum type");

    const Json::Value &fields = sentence["fields"];

    if (fields.size() != (parts.size() - 1))
        throw runtime_error("Incorrect number of fields");

    extract_fields(data, fields, parts);
    post_filters(data, sentence);
}

/* crude_parse is based on the parse() method of
 * habitat.parser_modules.ukhas_parser.UKHASParser */
Json::Value UKHASExtractor::crude_parse()
{
    const Json::Value *settings_ptr = mgr->payload();

    if (!settings_ptr)
        settings_ptr = &(Json::Value::null);
    else if (!settings_ptr->isObject())
        throw runtime_error("Invalid configuration: "
                "settings is not an object");

    const Json::Value &settings = *settings_ptr;

    string data, checksum;
    split_string(buffer, &data, &checksum);

    /* Warning: cpp_connector only supports xor and crc16-ccitt, which
     * conveninently are different lengths, so this works. */
    string checksum_name = examine_checksum(data, checksum);
    vector<string> parts = split(data, ',');
    if (!parts.size() || !parts[0].size())
        throw runtime_error("Empty callsign");

    Json::Value basic(Json::objectValue);
    cook_basic(basic, buffer, parts[0]);
    const Json::Value &sentences = settings["sentences"];

    if (!sentences.isNull())
    {
        if (!sentences.isArray())
            throw runtime_error("Invalid configuration: "
                    "sentences is not an array");

        /* Silence errors, and only log them if all attempts fail */
        vector<string> errors;

        for (Json::Value::const_iterator it = sentences.begin();
             it != sentences.end(); it++)
        {
            try
            {
                Json::Value data(basic);
                attempt_settings(data, (*it), checksum_name, parts);
                return data;
            }
            catch (runtime_error &e)
            {
                errors.push_back(e.what());
            }
        }

        /* Couldn't parse using any of the settings... */
        mgr->status("UKHAS Extractor: full parse failed:");
        for (vector<string>::const_iterator it = errors.begin();
             it != errors.end(); it++)
        {
            mgr->status("UKHAS Extractor: " + (*it));
        }
    }

    basic["_basic"] = true;
    return basic;
}

} /* namespace habitat */
