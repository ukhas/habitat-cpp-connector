# Copyright 2011-2012 (C) Daniel Richman. License: GNU GPL 3; see LICENSE

import subprocess
import os
import errno
import fcntl
import tempfile
import json
import BaseHTTPServer
import threading
import collections
import time
import uuid
import copy
import random
import xml.etree.cElementTree as ET
import urllib

from habitat.utils import rfc3339
from habitat import views

class ProxyException:
    def __init__(self, name, what=None):
        self.name = name
        self.what = what

    def __str__(self):
        return "ProxyException: {0.name}: {0.what!r}".format(self)

class Callbacks:
    def __init__(self):
        self.lock = threading.RLock()
        self.fake_time = 1300000000  # set in fake_main.cxx

    def advance_time(self, amount=1):
        with self.lock:
            self.fake_time += amount

    def time(self):
        with self.lock:
            return self.fake_time

    def fake_timestamp(self, value):
        """what the timestamp will be `value` fake seconds into the test"""
        return 1300000000 + value

    def fake_rfc3339(self, value):
        """what the local RFC3339 will be `value` fake seconds into the test"""
        return rfc3339.timestamp_to_rfc3339_localoffset(1300000000 + value)

class Proxy:
    def __init__(self, command, callsign, couch_uri=None, couch_db=None,
                 max_merge_attempts=None, callbacks=None, with_valgrind=False):

        self.closed = False
        self.blocking = True

        if with_valgrind:
            self.xmlfile = tempfile.NamedTemporaryFile("a+b")
            args = ("valgrind", "--quiet", "--xml=yes",
                    "--xml-file=" + self.xmlfile.name, command)
            self.p = subprocess.Popen(args, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE)
        else:
            self.xmlfile = None
            self.p = subprocess.Popen(command, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE)


        self.callbacks = callbacks
        self.re_init(callsign, couch_uri, couch_db, max_merge_attempts=None)

    def re_init(self, callsign, couch_uri=None, couch_db=None,
                max_merge_attempts=None):
        init_args = ["init", callsign]

        for a in [couch_uri, couch_db, max_merge_attempts]:
            if a is None:
                break
            init_args.append(a)

        self._proxy(init_args)

    def _write(self, command):
        s = json.dumps(command)
        print ">>", s
        self.p.stdin.write(s)
        self.p.stdin.write("\n")

    def _read(self):
        line = self.p.stdout.readline()
        assert line and line.endswith("\n")
        print "<<", line.strip()
        obj = json.loads(line)
        return obj

    def _proxy(self, command):
        self._write(command)
        return self.complete()

    def complete(self):
        while True:
            obj = self._read()

            if obj[0] == "return":
                if len(obj) == 1:
                    return None
                else:
                    return obj[1]
            elif obj[0] == "error":
                if len(obj) == 3:
                    raise ProxyException(obj[1], obj[2])
                else:
                    raise ProxyException(obj[1])
            elif obj[0] == "callback":
                if len(obj) == 3:
                    (cb, name, args) = obj
                else:
                    (cb, name) = obj
                    args = []
                func = getattr(self.callbacks, name)
                result = func(*args)

                self._write(["return", result])
            elif obj[0] == "log":
                pass
            else:
                raise AssertionError("invalid response")

    def unblock(self):
        assert self.blocking
        self.blocking = False

        fd = self.p.stdout.fileno()
        self.fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, self.fl | os.O_NONBLOCK)

    def block(self):
        assert not self.blocking
        self.blocking = True

        fd = self.p.stdout.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL, self.fl)

    def __del__(self):
        if not self.closed:
            self.close(check=False)

    def close(self, check=True):
        self.closed = True
        self.p.stdin.close()
        ret = self.p.wait()

        if check:
            assert ret == 0, ret
            self._check_valgrind()

    def _check_valgrind(self):
        if self.xmlfile:
            self.xmlfile.seek(0)
            tree = ET.parse(self.xmlfile)
            assert tree.find("error") == None

    def payload_telemetry(self, data, *args):
        return self._proxy(["payload_telemetry", data] + list(args))

    def listener_telemetry(self, data, *args):
        return self._proxy(["listener_telemetry", data] + list(args))

    def listener_information(self, data, *args):
        return self._proxy(["listener_information", data] + list(args))

    def flights(self):
        return self._proxy(["flights"])

    def payloads(self):
        return self._proxy(["payloads"])

    def reset(self):
        return self._proxy(["reset"])

temp_port = 55205

def next_temp_port():
    global temp_port
    temp_port += 1
    return temp_port

class MockHTTP(BaseHTTPServer.HTTPServer):
    def __init__(self, server_address=None, callbacks=None):
        if server_address == None:
            server_address = ('localhost', next_temp_port())

        BaseHTTPServer.HTTPServer.__init__(self, server_address,
                                           MockHTTPHandler)
        self.expecting = False
        self.expect_queue = collections.deque()
        self.url = "http://localhost:{0}".format(self.server_port)
        self.timeout = 1
        self.callbacks = callbacks

    def advance_time(self, value):
        self.callbacks.advance_time(value)

    expect_defaults = {
        # expect:
        "method": "GET",
        "path": "/",
        "body": None,   # string if you expect something from a POST
        # "body_json": {'object': True}
        "validate_body_json": True,

        # and respond with:
        "code": 404,
        "respond": "If this was a 200, this would be your page"
        # respond_json=object
    }

    def expect_request(self, **kwargs):
        assert not self.expecting

        e = self.expect_defaults.copy()
        e.update(kwargs)

        if "body_json" in e and e["validate_body_json"]:
            old = e.get("validate_old", None)
            new = e["body_json"]

            userctx = {'roles': []}
            secobj = {}

            for mod in [views.flight, views.listener_information,
                        views.listener_telemetry, views.payload_telemetry,
                        views.payload_configuration, views.habitat]:
                mod.validate(new, old, userctx, secobj)

        self.expect_queue.append(e)

    def run(self):
        assert not self.expecting
        self.expecting = True
        self.expect_handled = False
        self.expect_successes = 0
        self.expect_length = len(self.expect_queue)

        self.expect_thread = threading.Thread(target=self._run_expect)
        self.expect_thread.daemon = True
        self.expect_thread.start()

    def check(self):
        assert self.expecting
        self.expect_queue.clear()
        self.expect_thread.join()

        assert self.expect_successes == self.expect_length

        self.expecting = False

    def _run_expect(self):
        self.error = None
        while len(self.expect_queue):
            self.handle_request()

class MockHTTPHandler(BaseHTTPServer.BaseHTTPRequestHandler):
    def compare(self, a, b, what):
        if a != b:
            raise AssertionError("http request expect", what, a, b)

    def check_expect(self):
        print "-- HTTP " + self.command + " " + self.path

        assert self.server.expecting
        e = self.server.expect_queue.popleft()

        self.compare(e["method"], self.command, "method")
        self.compare(e["path"], urllib.unquote(self.path), "path")

        expect_100_header = self.headers.getheader('expect')
        expect_100 = expect_100_header and \
                     expect_100_header.lower() == "100-continue"
        support_100 = self.request_version != 'HTTP/0.9'

        if support_100 and expect_100:
            self.wfile.write(self.protocol_version + " 100 Continue\r\n\r\n")
            # See issue dl-fldigi#30
            raise AssertionError("Client used 100-continue!")

        length = self.headers.getheader('content-length')
        if length:
            length = int(length)
            body = self.rfile.read(length)
            assert len(body) == length
        else:
            body = None

        if "body_json" in e:
            self.compare(e["body_json"], json.loads(body), "body_json")
        else:
            self.compare(e["body"], body, "body")

        code = e["code"]
        if "respond_json" in e:
            content = json.dumps(e["respond_json"])
        else:
            content = e["respond"]

        if "wait" in e:
            e["wait"].set()

        if "delay" in e:
            e["delay"].wait()

        if "advance_time_after" in e:
            self.server.advance_time(e["advance_time_after"])

        self.send_response(code)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

        self.server.expect_successes += 1

    def log_request(self, *args, **kwargs):
        pass

    do_POST = check_expect
    do_GET = check_expect
    do_PUT = check_expect

class TestCPPConnector:
    command = "tests/cpp_connector"

    def setup(self):
        self.callbacks = Callbacks()
        self.couchdb = MockHTTP(callbacks=self.callbacks)
        self.uploader = Proxy(self.command, "PROXYCALL", self.couchdb.url,
                              callbacks=self.callbacks, with_valgrind=False)
        self.uuids = collections.deque()

        self.db_path = "/habitat/"

    def teardown(self):
        self.uploader.close()
        self.couchdb.server_close()

    def gen_fake_uuid(self):
        return str(uuid.uuid1()).replace("-", "")

    def gen_fake_rev(self, num=1):
        return str(num) + "-" + self.gen_fake_uuid()

    def expect_uuid_request(self):
        new_uuids = [self.gen_fake_uuid() for i in xrange(100)]
        self.uuids.extend(new_uuids)

        self.couchdb.expect_request(
            path="/_uuids?count=100",
            code=200,
            respond_json={"uuids": new_uuids},
        )

    def pop_uuid(self):
        self.ensure_uuids()
        return self.uuids.popleft()

    def ensure_uuids(self):
        if not len(self.uuids):
            self.expect_uuid_request()

    def expect_save_doc(self, doc, rev=None, **kwargs):
        if not rev:
            rev = 1

        self.couchdb.expect_request(
            method="PUT",
            path=self.db_path + doc["_id"],
            body_json=doc,
            code=201,
            respond_json={"id": doc["_id"], "rev": self.gen_fake_rev(rev)},
            **kwargs
        )

    def expect_add_listener_update(self, doc_id, protodoc, **kwargs):
        if "code" not in kwargs:
            kwargs["code"] = 200
        if "respond" not in kwargs and "respond_json" not in kwargs:
            kwargs["respond"] = "OK"

        self.couchdb.expect_request(
            method="PUT",
            path=self.db_path + "_design/payload_telemetry/" +
                    "_update/add_listener/" + doc_id,
            body_json=protodoc,
            validate_body_json=False,
            **kwargs
        )

    def test_uses_server_uuids(self):
        should_use_uuids = []

        for i in xrange(200):
            uuid = self.pop_uuid()
            should_use_uuids.append(uuid)

            doc = {
                "_id": uuid,
                "time_created": self.callbacks.fake_rfc3339(i),
                "time_uploaded": self.callbacks.fake_rfc3339(i),
                "data": {
                    "callsign": "PROXYCALL",
                    "latitude": 3.12,
                    "longitude": -123.1
                },
                "type": "listener_telemetry"
            }

            self.expect_save_doc(doc, advance_time_after=1)

        self.couchdb.run()

        data = {
            "latitude": 3.12,
            "longitude": -123.1
        }

        for i in xrange(200):
            doc_id = self.uploader.listener_telemetry(data)
            assert doc_id == should_use_uuids[i]

        self.couchdb.check()

    def add_sample_listener_docs(self):
        telemetry_data = {"latitude": 1.0, "longitude": 2.0,
                          "some_data": 123, "_flag": True}
        telemetry_doc = {
            "_id": self.pop_uuid(),
            "data": copy.deepcopy(telemetry_data),
            "type": "listener_telemetry",
            "time_created": self.callbacks.fake_rfc3339(0),
            "time_uploaded": self.callbacks.fake_rfc3339(0)
        }
        telemetry_doc["data"]["callsign"] = "PROXYCALL"

        info_data = {"my_radio": "Duga-3", "vehicle": "Tractor"}
        info_doc = {
            "_id": self.pop_uuid(),
            "data": copy.deepcopy(info_data),
            "type": "listener_information",
            "time_created": self.callbacks.fake_rfc3339(0),
            "time_uploaded": self.callbacks.fake_rfc3339(0)
        }
        info_doc["data"]["callsign"] = "PROXYCALL"

        self.expect_save_doc(telemetry_doc)
        self.expect_save_doc(info_doc)

        self.couchdb.run()
        self.sample_telemetry_doc_id = \
                self.uploader.listener_telemetry(telemetry_data)
        self.sample_info_doc_id = self.uploader.listener_information(info_data)
        self.couchdb.check()

        assert self.sample_telemetry_doc_id == telemetry_doc["_id"]
        assert self.sample_info_doc_id == info_doc["_id"]

    def test_pushes_listener_docs(self):
        self.add_sample_listener_docs()

        # And now again, but this time, setting time_created.
        telemetry_data = {
            "time": "12:40:05",
            "latitude": 35.11,
            "longitude": 137.567,
            "altitude": 12
        }
        telemetry_doc = {
            "_id": self.pop_uuid(),
            "data": copy.deepcopy(telemetry_data),
            "type": "listener_telemetry",
            "time_created": self.callbacks.fake_rfc3339(501),
            "time_uploaded": self.callbacks.fake_rfc3339(1000)
        }
        telemetry_doc["data"]["callsign"] = "PROXYCALL"

        info_data = {
            "name": "Daniel Richman",
            "location": "Reading, UK",
            "radio": "Yaesu FT 790R",
            "antenna": "Whip"
        }
        info_doc = {
            "_id": self.pop_uuid(),
            "data": copy.deepcopy(info_data),
            "type": "listener_information",
            "time_created": self.callbacks.fake_rfc3339(409),
            "time_uploaded": self.callbacks.fake_rfc3339(1005)
        }
        info_doc["data"]["callsign"] = "PROXYCALL"

        self.expect_save_doc(telemetry_doc, advance_time_after=5)
        self.expect_save_doc(info_doc)

        self.callbacks.advance_time(1000)
        self.couchdb.run()
        telemetry_doc_id = \
            self.uploader.listener_telemetry(telemetry_data,
                    self.callbacks.fake_timestamp(501))
        info_doc_id = self.uploader.listener_information(info_data,
                    self.callbacks.fake_timestamp(409))
        self.couchdb.check()

        assert telemetry_doc_id == telemetry_doc["_id"]
        assert info_doc_id == info_doc["_id"]

    ptlm_doc_id = "c0be13b259acfd2fe23cd0d1e70555d6" \
                  "8f83926278b23f5b813bdc75f6b9cdd6"
    ptlm_string = "asdf blah \x12 binar\x04\x01 asdfasdfsz"
    ptlm_metadata = {"frequency": 434075000, "misc": "Hi"}
    ptlm_doc_ish = {
        "data": {
            "_raw": "YXNkZiBibGFoIBIgYmluYXIEASBhc2RmYXNkZnN6"
        },
        "receivers": {
            "PROXYCALL": {
                "frequency": 434075000,
                "misc": "Hi"
            }
        }
    }

    def make_ptlm_doc_ish(self, time_created=0, time_uploaded=0, **extra):
        receiver_info = {
            "time_created": self.callbacks.fake_rfc3339(time_created),
            "time_uploaded": self.callbacks.fake_rfc3339(time_uploaded),
        }
        receiver_info.update(extra)

        doc_ish = copy.deepcopy(self.ptlm_doc_ish)
        doc_ish["receivers"]["PROXYCALL"].update(receiver_info)

        return doc_ish

    def test_payload_telemetry_simple(self):
        # WARNING: JsonCPP does not support strings with \0 in the middle of
        # them, because it does not store the length of the string and instead
        # later figures it out with strlen. This does not harm the uploader
        # because our code converts binary data to base64 before giving it
        # to the json encoder. However, the json stdin proxy call interface
        # isn't going to work with nulls in it.

        doc_ish = self.make_ptlm_doc_ish()

        self.expect_add_listener_update(self.ptlm_doc_id, doc_ish)
        self.couchdb.run()

        ret_doc_id = self.uploader.payload_telemetry(self.ptlm_string,
                                                     self.ptlm_metadata)
        self.couchdb.check()

        assert ret_doc_id == self.ptlm_doc_id

    def test_adds_latest_listener_doc(self):
        self.add_sample_listener_docs()

        doc_ish = self.make_ptlm_doc_ish(
            latest_listener_telemetry=self.sample_telemetry_doc_id,
            latest_listener_information=self.sample_info_doc_id
        )

        self.expect_add_listener_update(self.ptlm_doc_id, doc_ish)
        self.couchdb.run()
        self.uploader.payload_telemetry(self.ptlm_string, self.ptlm_metadata)
        self.couchdb.check()

    def test_ptlm_retries_conflicts(self):
        doc_ish = self.make_ptlm_doc_ish()

        self.expect_add_listener_update(
            self.ptlm_doc_id, doc_ish,
            code=409,
            respond_json={"error": "conflict"},
            advance_time_after=5
        )

        doc_ish = copy.deepcopy(doc_ish)
        doc_ish["receivers"]["PROXYCALL"]["time_uploaded"] = \
                self.callbacks.fake_rfc3339(5)

        self.expect_add_listener_update(self.ptlm_doc_id, doc_ish)

        self.couchdb.run()
        self.uploader.payload_telemetry(self.ptlm_string, self.ptlm_metadata)
        self.couchdb.check()

    def test_ptlm_doesnt_retry_other_errors(self):
        yield self.check_ptlm_doesnt_retry_code, 401
        yield self.check_ptlm_doesnt_retry_code, 403

    def check_ptlm_doesnt_retry_code(self, code):
        doc_ish = self.make_ptlm_doc_ish()

        self.expect_add_listener_update(
            self.ptlm_doc_id, doc_ish,
            code=code,
            respond_json={"error": "of some sort"},
            advance_time_after=5
        )

        self.couchdb.run()

        try:
            self.uploader.payload_telemetry(self.ptlm_string,
                                            self.ptlm_metadata)
        except ProxyException, e:
            if e.name == "runtime_error" and \
               e.what == "habitat::UnmergeableError":
                pass
            else:
                raise
        else:
            raise AssertionError("Did not raise UnmergeableError")

        self.couchdb.check()

    def add_mock_conflicts(self, n):
        doc_ish = self.make_ptlm_doc_ish()

        self.expect_add_listener_update(
            self.ptlm_doc_id, doc_ish,
            code=409,
            respond_json={"error": "conflict"},
            advance_time_after=1
        )
            
        for i in xrange(n):
            doc_ish = copy.deepcopy(doc_ish)
            doc_ish["receivers"]["PROXYCALL"]["time_uploaded"] = \
                self.callbacks.fake_rfc3339(i + 1)

            self.expect_add_listener_update(
                self.ptlm_doc_id, doc_ish,
                code=409,
                respond_json={"error": "conflict"},
                advance_time_after=1
            )

    def test_merges_multiple_conflicts(self):
        self.add_mock_conflicts(15)
        doc_ish = self.make_ptlm_doc_ish(time_created=0, time_uploaded=16)
        self.expect_add_listener_update(self.ptlm_doc_id, doc_ish)

        self.couchdb.run()
        self.uploader.payload_telemetry(self.ptlm_string, self.ptlm_metadata)
        self.couchdb.check()

    def test_gives_up_after_many_conflicts(self):
        self.add_mock_conflicts(19)
        self.couchdb.run()

        try:
            self.uploader.payload_telemetry(self.ptlm_string,
                                            self.ptlm_metadata)
        except ProxyException, e:
            if e.name == "runtime_error" and \
               e.what == "habitat::UnmergeableError":
                pass
            else:
                raise
        else:
            raise AssertionError("Did not raise UnmergeableError")

    def test_update_func_likes_doc(self):
        doc_ish = self.make_ptlm_doc_ish()

        req = {
            "body": json.dumps(doc_ish),
            "id": self.ptlm_doc_id
        }
        views.payload_telemetry.add_listener_update(None, req)

    def test_flights(self):
        rows = []
        expect_result = []
        pcfgs = []

        for i in xrange(100):
            pcfgs.append({"_id": "pcfg_{0}".format(i),
                          "type": "payload_configuration", "i": i})
        for i in xrange(20):
            pcfgs.append({"_id": "nonexistant_{0}".format(i)})
        for i in xrange(100):
            payloads = random.sample(pcfgs, random.randint(1, 5))
            f_id = "flight_{0}".format(i)
            doc = {"_id": f_id, "type": "flight", "i": i,
                   "payloads": [p["_id"] for p in payloads]}

            start = self.callbacks.fake_rfc3339(1000 + i)
            end = self.callbacks.fake_rfc3339(2000 + i)
            rows.append({"id": f_id, "key": [end, start, f_id, 0],
                        "value": None, "doc": doc})

            expect_payloads = []
            for p in payloads:
                p_id = p["_id"]
                if "nonexistant" in p_id:
                    # nonexistant referenced docs should not be in 
                    # _payload_docs
                    p = None
                else:
                    expect_payloads.append(p)

                rows.append({"id": f_id, "key": [end, start, f_id, 1],
                            "value": {"_id": p_id}, "doc": p})

            doc = copy.deepcopy(doc)
            doc["_payload_docs"] = expect_payloads
            expect_result.append(doc)

        fake_view_response = \
                {"total_rows": len(rows), "offset": 0, "rows": rows}

        self.callbacks.advance_time(1925)
        view_time = self.callbacks.fake_timestamp(1925)
        view_path = "_design/flight/_view/end_start_including_payloads"
        options = "include_docs=true&startkey=[{0}]".format(view_time)

        self.couchdb.expect_request(
            path=self.db_path + view_path + "?" + options,
            code=200,
            respond_json=copy.deepcopy(fake_view_response)
        )
        self.couchdb.run()

        result = self.uploader.flights()
        assert result == expect_result

    def test_payloads(self):
        payloads = [{"_id": "pcfg_{0}".format(i), "a flight": i}
                  for i in xrange(100)]
        rows = [{"id": doc["_id"], "key": None, "value": None, "doc": doc}
                for doc in payloads]
        fake_view_response = \
                {"total_rows": len(rows), "offset": 0, "rows": rows}

        view_path = "_design/payload_configuration/_view/name_time_created"
        options = "include_docs=true"

        self.couchdb.expect_request(
            path=self.db_path + view_path + "?" + options,
            code=200,
            respond_json=copy.deepcopy(fake_view_response)
        )
        self.couchdb.run()

        result = self.uploader.payloads()
        assert result == payloads

class TestCPPConnectorThreaded(TestCPPConnector):
    command = "tests/cpp_connector_threaded"

    def test_queues_things(self):
        telemetry_data = {"this was queued": True,
                          "latitude": 1.0, "longitude": 2.0}
        telemetry_doc = {
            "_id": self.pop_uuid(),
            "data": copy.deepcopy(telemetry_data),
            "type": "listener_telemetry",
            "time_created": self.callbacks.fake_rfc3339(0),
            "time_uploaded": self.callbacks.fake_rfc3339(0)
        }
        telemetry_doc["data"]["callsign"] = "PROXYCALL"

        info_data = {"5": "this was the second item in the queue"}
        info_doc = {
            "_id": self.pop_uuid(),
            "data": copy.deepcopy(info_data),
            "type": "listener_information",
            "time_created": self.callbacks.fake_rfc3339(0),
            "time_uploaded": self.callbacks.fake_rfc3339(0)
        }
        info_doc["data"]["callsign"] = "PROXYCALL"

        delay_one = threading.Event()
        wait_one = threading.Event()
        delay_two = threading.Event()
        wait_two = threading.Event()

        self.expect_save_doc(telemetry_doc, delay=delay_one, wait=wait_one)
        self.expect_save_doc(info_doc, delay=delay_two, wait=wait_two)

        self.couchdb.run()

        self.run_unblocked(self.uploader.listener_telemetry, telemetry_data)
        self.run_unblocked(self.uploader.listener_information, info_data)

        # The complexity of doing this properly justifies this evil hack...
        # right?
        while not wait_one.is_set():
            wait_one.wait(0.1)
            self.run_unblocked(self.uploader.complete)

        delay_one.set()
        assert self.uploader.complete() == telemetry_doc["_id"]

        while not wait_two.is_set():
            wait_two.wait(0.01)
            self.run_unblocked(self.uploader.complete)

        delay_two.set()
        assert self.uploader.complete() == info_doc["_id"]

        self.couchdb.check()

    def run_unblocked(self, func, *args, **kwargs):
        self.uploader.unblock()

        try:
            return func(*args, **kwargs)
        except IOError as e:
            if e.errno != errno.EAGAIN:
                raise
        else:
            raise AssertionError("expected IOError(EAGAIN)")

        self.uploader.block()

    def test_changes_settings(self):
        self.uploader.re_init("NEWCALL", self.couchdb.url)

        receiver_info = {
            "time_created": self.callbacks.fake_rfc3339(0),
            "time_uploaded": self.callbacks.fake_rfc3339(0),
        }

        doc_ish = self.make_ptlm_doc_ish()
        doc_ish["receivers"]["NEWCALL"] = doc_ish["receivers"]["PROXYCALL"]
        del doc_ish["receivers"]["PROXYCALL"]

        self.expect_add_listener_update(self.ptlm_doc_id, doc_ish)
        self.couchdb.run()

        ret_doc_id = self.uploader.payload_telemetry(self.ptlm_string,
                                                     self.ptlm_metadata)
        self.couchdb.check()

        assert ret_doc_id == self.ptlm_doc_id

    def test_reset(self):
        self.uploader.re_init("NEWCALL", self.couchdb.url)
        self.uploader.reset()

        self.couchdb.run()  # expect nothing.

        try:
            self.uploader.payload_telemetry("asdf", {})
        except ProxyException as e:
            assert "NotInitialised" in str(e)
        else:
            raise AssertionError("not initialised was not thrown")

        self.couchdb.check()
