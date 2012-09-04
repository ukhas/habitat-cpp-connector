/* Copyright 2012 (C) Daniel Richman. License: GNU GPL 3; see LICENSE. */

#include "habitat/RFC3339.h"

#include <string>
#include <sstream>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <iomanip>

/* on mingw32, localtime_r and gmtime_r don't exist. Instead, pthreadsw32's
 * pthread.h provides macros that map calls from localtime_r to localtime(),
 * (and the same for gmtime), noting that "[the] WIN32 C runtime library has
 * been made thread-safe without affecting the user interface."
 *
 * tl;dr: need to include pthread.h to get localtime_r on mingw32 */
#include <pthread.h>

using namespace std;

namespace RFC3339 {

bool validate_rfc3339(const string &rfc3339)
{
    try
    {
        rfc3339_to_timestamp(rfc3339);
    }
    catch (InvalidFormat &e)
    {
        return false;
    }

    return true;
}

/* Class to be used when extracting from an istream that consumes a single
 * delimiter character */
class Delim
{
    char expect;

public:
    Delim(char _e) : expect(_e) {};
    ~Delim() {};
    void extract(istream &in);
};

/* Delim can't be a reference here because in its intended use it is
 * constructed on the spot like this ... >> Delim('-') >> ... and therefore
 * it does not have an address */
istream & operator>>(istream &in, Delim delim)
{
    delim.extract(in);
    return in;
}

void Delim::extract(istream &in)
{
    if (!in.good() || in.get() != expect)
        in.setstate(ios_base::badbit);
}

/* Extracts an integer of particular length, containing the digits 0-9 only */
class StrictInt
{
    size_t length;
    int &target;
    bool check_range;
    int min, max;

public:
    StrictInt(size_t _l, int &_t)
        : length(_l), target(_t), check_range(false), min(0), max(0) {};
    StrictInt(size_t _l, int &_t, int _min, int _max)
        : length(_l), target(_t), check_range(true), min(_min), max(_max) {};
    ~StrictInt() {};
    void extract(istream &in);
};

istream & operator>>(istream &in, StrictInt tgt)
{
    tgt.extract(in);
    return in;
}

void StrictInt::extract(istream &in)
{
    if (!in.good())
    {
        in.setstate(ios_base::badbit);
        return;
    }

    char *temp = new char[length + 1];
    in.read(temp, length);
    temp[length] = 0;

    if (in.fail() || strlen(temp) != length)
        return;

    istringstream temp_ss(temp);
    temp_ss >> target;

    if (temp_ss.fail() || temp_ss.peek() != EOF)
        in.setstate(ios_base::badbit);

    if (check_range && (target < min || target > max))
        in.setstate(ios_base::badbit);
}

static int mdays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
static int mydays[] = {0, 0, 31, 59, 90, 120, 151, 181, 212,
                       243, 273, 304, 334};

/* Returns the number of multiples of n in [a,b] */
static int multiples_between(int n, int a, int b)
{
    if (a % n != 0)
        a += n - (a % n);
    b -= (b % n);
    return ((b - a) / n) + 1;
}

/* Not provided on all platforms :-(. */
static long long int my_timegm(int year, int month, int mday,
                               int hour, int min, int sec)
{
    /* I don't know the best way to get everything promoted to 64bit
     * integers in the final line, this might do it */
    long long int epoch_days = 0;
    bool leap_year = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));

    if (year > 1970)
    {
        int leap_years = multiples_between(4, 1970, year - 1)
                       - multiples_between(100, 1970, year - 1)
                       + multiples_between(400, 1970, year - 1);
        epoch_days = ((year - 1970) * 365) + leap_years;
    }
    else if (year < 1970)
    {
        int leap_years = multiples_between(4, year, 1970 - 1)
                       - multiples_between(100, year, 1970 - 1)
                       + multiples_between(400, year, 1970 - 1);
        epoch_days = -(((1970 - year) * 365) + leap_years);
    }

    epoch_days += mydays[month];
    if (month > 2 && leap_year)
        epoch_days++;
    epoch_days += mday - 1;

    return (((((epoch_days * 24) + hour) * 60) + min) * 60) + sec;
}

static long long int my_timegm(struct tm tm)
{
    return my_timegm(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Thin wrappers adding exceptions and range checks */
static struct tm my_localtime(long long int timestamp)
{
    time_t timestamp_s = timestamp;
    if (timestamp_s != timestamp)
        throw out_of_range("timestamp too large for time_t");

    struct tm tm;
    if (localtime_r(&timestamp_s, &tm) == NULL)
        throw runtime_error("localtime_r failed");

    return tm;
}

static struct tm my_gmtime(long long int timestamp)
{
    time_t timestamp_s = timestamp;
    if (timestamp_s != timestamp)
        throw out_of_range("timestamp too large for time_t");

    struct tm tm;
    if (gmtime_r(&timestamp_s, &tm) == NULL)
        throw runtime_error("gmtime_r failed");

    return tm;
}

long long int rfc3339_to_timestamp(const string &rfc3339)
{
    istringstream temp(rfc3339);

    int year, month, mday, hour, min, sec;

    temp >> StrictInt(4, year) >> Delim('-')
        >> StrictInt(2, month, 1, 12) >> Delim('-')
        >> StrictInt(2, mday, 1, 31) >> Delim('T')
        >> StrictInt(2, hour, 0, 23) >> Delim(':')
        >> StrictInt(2, min, 0, 59) >> Delim(':')
        >> StrictInt(2, sec, 0, 59);

    if (temp.fail() || temp.eof() || temp.tellg() != 19)
        throw InvalidFormat();

    bool leap_year = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int this_mdays = mdays[month];

    if (leap_year && month == 2)
        this_mdays++;

    if (mday > this_mdays)
        throw InvalidFormat();

    if (temp.peek() == '.')
    {
        /* discard seconds part */
        do
            temp.get();
        while (temp.good() && temp.peek() >= '0' && temp.peek() <= '9');
    }

    int offset = 0;
    int offset_first_char = temp.get();

    if (offset_first_char == 'Z')
    {
        /* UTC offset, 0 */
    }
    else if (offset_first_char == '+' || offset_first_char == '-')
    {
        int offset_hours, offset_minutes;
        temp >> StrictInt(2, offset_hours, 0, 23) >> Delim(':')
            >> StrictInt(2, offset_minutes, 0, 59);

        if (temp.fail())
            throw InvalidFormat();

        offset = offset_hours * 3600 + offset_minutes * 60;

        if (offset_first_char == '-')
            offset = -offset;
    }
    else
    {
        throw InvalidFormat();
    }

    if (temp.peek() != EOF)
        throw InvalidFormat();

    return my_timegm(year, month, mday, hour, min, sec) - offset;
}

static string make_datestring_start(int year, int month, int mday,
                                    int hour, int min, int sec)
{
    ostringstream temp;
    temp.fill('0');

    temp << setw(4) << year << '-'
        << setw(2) << month << '-'
        << setw(2) << mday << 'T'
        << setw(2) << hour << ':'
        << setw(2) << min << ':'
        << setw(2) << sec;

    return temp.str();
}

static string make_datestring_start(struct tm tm)
{
    return make_datestring_start(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                 tm.tm_hour, tm.tm_min, tm.tm_sec);
}

string timestamp_to_rfc3339_utcoffset(long long int timestamp)
{
    struct tm tm = my_gmtime(timestamp);
    string ret = make_datestring_start(tm) + "Z";
#ifndef NDEBUG
    if (rfc3339_to_timestamp(ret) != timestamp)
        throw runtime_error("reparse sanity check failed");
#endif
    return ret;
}

string timestamp_to_rfc3339_localoffset(long long int timestamp)
{
    struct tm tm = my_localtime(timestamp);
    struct tm gm_tm = my_gmtime(timestamp);

    int offset = my_timegm(tm) - my_timegm(gm_tm);

    if (abs(offset) % 60 != 0)
        throw runtime_error("Your local offset is not a whole minute");

    int offset_minutes = abs(offset) / 60;
    int offset_hours = offset_minutes / 60;
    offset_minutes %= 60;

    ostringstream temp;
    temp << make_datestring_start(tm);
    temp.fill('0');

    temp << (offset < 0 ? '-' : '+')
        << setw(2) << offset_hours << ':'
        << setw(2) << offset_minutes;

    string ret = temp.str();

#ifndef NDEBUG
    if (rfc3339_to_timestamp(ret) != timestamp)
        throw runtime_error("reparse sanity check failed");
#endif
    return ret;
}

string now_to_rfc3339_utcoffset()
{
    return timestamp_to_rfc3339_utcoffset(time(NULL));
}

string now_to_rfc3339_localoffset()
{
    return timestamp_to_rfc3339_localoffset(time(NULL));
}

} /* namespace RFC3339 */
