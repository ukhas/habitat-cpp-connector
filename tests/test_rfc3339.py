# Copyright 2012 (C) Adam Greig, Daniel Richman
#
# This file is part of habitat.
#
# habitat is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# habitat is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with habitat.  If not, see <http://www.gnu.org/licenses/>.

import os
import time
import subprocess
import json
from nose import SkipTest

# Identical to habitat's rfc3339 tests, except with a class that proxies
# calls to an executable and without the float tests.

class ProxyInvalidFormat(ValueError):
    pass


class ProxyFunction(object):
    def __init__(self, process, name):
        self.p = process
        self.name = name

    def __call__(self, *args):
        args = [self.name] + list(args)
        self.p.stdin.write(json.dumps(args))
        self.p.stdin.write("\n")
        response, thing = json.loads(self.p.stdout.readline())
        if response == "return":
            return thing
        elif response == "time_t error":
            raise ValueError("timestamp out of range for platform time_t")
        elif thing == "RFC3339::InvalidFormat":
            raise ProxyInvalidFormat
        else:
            raise Exception(thing)


class ProxyFunctionModule(object):
    # i.e., a module that contains functions only
    def __init__(self, command, environ=None):
        self.closed = False
        self.p = subprocess.Popen(command, env=environ,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE)

        for n in ["validate_rfc3339", "rfc3339_to_timestamp",
                  "timestamp_to_rfc3339_utcoffset",
                  "timestamp_to_rfc3339_localoffset",
                  "now_to_rfc3339_utcoffset",
                  "now_to_rfc3339_localoffset"]:
            setattr(self, n, ProxyFunction(self.p, n))

    def close(self, quiet=False):
        self.p.stdin.close()
        ret = self.p.wait()

        if not quiet:
            assert ret == 0

    def __del__(self):
        if not self.closed:
            self.close(True)


class TestValidateRFC3339(object):
    def setup(self):
        self.mod = ProxyFunctionModule("tests/rfc3339")
        self.validate = self.mod.validate_rfc3339

    def teardown(self):
        self.mod.close()

    def test_rejects_bad_format(self):
        assert not self.validate("asdf")
        assert not self.validate("24822")
        assert not self.validate("123-345-124T123:453:213")
        assert not self.validate("99-09-12T12:42:21Z")
        assert not self.validate("99-09-12T12:42:21+00:00")
        assert not self.validate("1999-09-12T12:42:21+00:")
        assert not self.validate("2012-09-12T21:-1:21")

    def test_rejects_no_offset(self):
        assert not self.validate("2012-09-12T12:42:21")

    def test_rejects_out_of_range(self):
        assert not self.validate("2012-00-12T12:42:21Z")
        assert not self.validate("2012-13-12T12:42:21Z")
        assert not self.validate("2012-09-00T12:42:21Z")
        assert not self.validate("2012-09-31T12:42:21Z")   # Sep
        assert self.validate("2012-08-31T12:42:21Z")       # Aug
        assert not self.validate("2012-08-32T12:42:21Z")
        assert not self.validate("2012-09-12T24:00:00Z")
        assert not self.validate("2012-09-12T12:60:21Z")
        assert not self.validate("2012-09-12T12:42:99Z")
        assert not self.validate("2012-09-12T12:42:21+24:00")
        assert not self.validate("2012-09-12T12:42:21-24:00")
        assert not self.validate("2012-09-12T12:42:21+02:60")

    def test_rejects_leap_seconds(self):
        # with regret :-(
        assert not self.validate("2012-06-30T23:59:60Z")
        assert not self.validate("2012-03-21T09:21:60Z")

    def test_handles_leapyear(self):
        assert self.validate("2012-02-29T12:42:21Z")
        assert not self.validate("2012-02-30T12:42:21Z")
        assert self.validate("2000-02-29T12:42:21Z")
        assert not self.validate("2000-02-30T12:42:21Z")
        assert self.validate("2100-02-28T12:42:21Z")
        assert not self.validate("2100-02-29T12:42:21Z")
        assert self.validate("2011-02-28T12:42:21Z")
        assert not self.validate("2011-02-29T12:42:21Z")

    def test_accepts_good(self):
        assert self.validate("1994-03-14T17:00:00Z")
        assert self.validate("2011-06-23T17:12:00+05:21")
        assert self.validate("1992-03-14T17:04:00-01:42")


class TestRFC3339toTimestamp(object):
    def setup(self):
        self.mod = ProxyFunctionModule("tests/rfc3339")
        self.func = self.mod.rfc3339_to_timestamp

    def teardown(self):
        self.mod.close()

    def test_validates(self):
        # This string would otherwise work: it would blissfully add the
        # offset
        try:
            self.func("2012-09-12T12:42:21+24:00")
        except ProxyInvalidFormat:
            pass
        else:
            raise Exception("Didn't throw InvalidFormat")

    def test_simple_cases(self):
        assert self.func("1996-12-19T16:39:57-08:00") == 851042397
        assert self.func("2012-08-08T21:30:36+01:00") == 1344457836
        assert self.func("1994-03-14T17:00:00Z") == 763664400
        assert self.func("1970-01-01T00:00:00Z") == 0
        assert self.func("1970-01-01T01:20:34Z") == 4834
        assert self.func("1969-12-31T23:59:59Z") == -1
        assert self.func("1969-12-31T22:51:12Z") == -4128

    def test_y2038(self):
        assert self.func("2100-01-01T00:00:00Z") == 4102444800
        assert self.func("1900-01-01T00:00:00Z") == -2208988800

    def test_leap_year(self):
        assert self.func("2012-02-29T12:42:21Z") == 1330519341
        assert self.func("2000-02-29T23:59:59Z") == 951868799
        assert self.func("2011-02-28T04:02:12Z") == 1298865732
        assert self.func("2100-02-28T00:00:00Z") == 4107456000
        assert self.func("1900-02-28T00:00:00Z") == -2203977600

    def test_dst_transition(self):
        assert self.func("2012-03-25T00:59:59+00:00") == 1332637199
        assert self.func("2012-03-25T02:00:00+01:00") == 1332637200
        assert self.func("2012-10-28T01:00:00+01:00") == 1351382400
        assert self.func("2012-10-28T01:00:00+00:00") == 1351386000

    def test_wacky_offset(self):
        assert self.func("1996-12-19T16:39:57-02:52") == 851042397 - 18480
        assert self.func("2012-08-08T21:30:36+23:11") == 1344457836 - 79860

    def test_float(self):
        assert self.func("1996-12-19T16:39:57.1234-08:00") == 851042397
        assert self.func("1996-12-20T00:39:57.004Z") == 851042397


class TestTimestampToRFC3339UTCOffset(object):
    def setup(self):
        self.mod = ProxyFunctionModule("tests/rfc3339")
        self.func = self.mod.timestamp_to_rfc3339_utcoffset

    def teardown(self):
        self.mod.close()


    def test_simple_cases(self):
        assert self.func(851042397) == "1996-12-20T00:39:57Z"
        assert self.func(1344457836) == "2012-08-08T20:30:36Z"
        assert self.func(763664400) == "1994-03-14T17:00:00Z"
        assert self.func(0) == "1970-01-01T00:00:00Z"
        assert self.func(4834) == "1970-01-01T01:20:34Z"
        assert self.func(-1) == "1969-12-31T23:59:59Z"
        assert self.func(-4128) == "1969-12-31T22:51:12Z"

    def test_y2038(self):
        try:
            assert self.func(4102444800) == "2100-01-01T00:00:00Z"
            assert self.func(-2208988800) == "1900-01-01T00:00:00Z"
        except ValueError as e:
            if str(e) == "timestamp out of range for platform time_t":
                raise SkipTest("Can't run this test on 32 bit")
            else:
                raise

    def test_leap_year(self):
        assert self.func(1330519341) == "2012-02-29T12:42:21Z"
        assert self.func(951868799) == "2000-02-29T23:59:59Z"
        assert self.func(1298865732) == "2011-02-28T04:02:12Z"

        try:
            assert self.func(4107456000) == "2100-02-28T00:00:00Z"
            assert self.func(-2203977600) == "1900-02-28T00:00:00Z"
        except ValueError as e:
            if str(e) == "timestamp out of range for platform time_t":
                raise SkipTest("Can't run this test on 32 bit")
            else:
                raise

    def test_now(self):
        s = self.mod.now_to_rfc3339_utcoffset()
        assert s[-1] == "Z"
        assert len(s) == 20
        d = int(time.time()) - self.mod.rfc3339_to_timestamp(s)
        assert d == 0 or d == 1


class TestTimestampToRFC3339LocalOffsetLondon(object):
    def setup(self):
        self.old = os.environ.get("TZ", None)
        os.environ["TZ"] = "Europe/London"
        time.tzset()

        self.mod = ProxyFunctionModule("tests/rfc3339", environ=os.environ)
        self.func = self.mod.timestamp_to_rfc3339_localoffset

    def teardown(self):
        if self.old is None:
            del os.environ["TZ"]
        else:
            os.environ["TZ"] = self.old
        time.tzset()

        self.mod.close()

    def test_simple_cases(self):
        assert self.func(851042397) == "1996-12-20T00:39:57+00:00"
        assert self.func(1344457836) == "2012-08-08T21:30:36+01:00"
        assert self.func(763664400) == "1994-03-14T17:00:00+00:00"
        assert self.func(1340280000) == "2012-06-21T13:00:00+01:00"
        assert self.func(1234) == "1970-01-01T01:20:34+01:00"
        assert self.func(-7728) == "1969-12-31T22:51:12+01:00"

    def test_dst_transition(self):
        assert self.func(1332637199) == "2012-03-25T00:59:59+00:00"
        assert self.func(1332637200) == "2012-03-25T02:00:00+01:00"
        assert self.func(1351382400) == "2012-10-28T01:00:00+01:00"
        assert self.func(1351386000) == "2012-10-28T01:00:00+00:00"

    def test_now(self):
        s = self.mod.now_to_rfc3339_localoffset()
        w = self.mod.rfc3339_to_timestamp(s)
        assert s[-6:] == ["+00:00", "+01:00"][time.localtime(w).tm_isdst]

        d = int(time.time()) - w
        assert d == 0 or d == 1


class TestTimestampToRFC3339LocalOffsetNewYork(object):
    def setup(self):
        self.old = os.environ.get("TZ", None)
        os.environ["TZ"] = "America/New_York"
        time.tzset()

        self.mod = ProxyFunctionModule("tests/rfc3339", environ=os.environ)
        self.func = self.mod.timestamp_to_rfc3339_localoffset

    def teardown(self):
        if self.old is None:
            del os.environ["TZ"]
        else:
            os.environ["TZ"] = self.old
        time.tzset()

        self.mod.close()

    def test_simple_cases(self):
        assert self.func(851042397) == "1996-12-19T19:39:57-05:00"
        assert self.func(1344457836) == "2012-08-08T16:30:36-04:00"
        assert self.func(761245200) == "1994-02-14T12:00:00-05:00"
        assert self.func(1340280000) == "2012-06-21T08:00:00-04:00"
        assert self.func(19234) == "1970-01-01T00:20:34-05:00"
        assert self.func(-4128) == "1969-12-31T17:51:12-05:00"

    def test_dst_transition(self):
        assert self.func(1331449199) == "2012-03-11T01:59:59-05:00"
        assert self.func(1331449200) == "2012-03-11T03:00:00-04:00"
        assert self.func(1352005200) == "2012-11-04T01:00:00-04:00"
        assert self.func(1352008800) == "2012-11-04T01:00:00-05:00"

    def test_now(self):
        s = self.mod.now_to_rfc3339_localoffset()
        w = self.mod.rfc3339_to_timestamp(s)
        assert s[-6:] == ["-05:00", "-04:00"][time.localtime(w).tm_isdst]

        d = int(time.time()) - w
        assert d == 0 or d == 1
