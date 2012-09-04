#!/usr/bin/make -f
# -*- makefile -*-
# Copyright 2011 (C) Daniel Richman. License: GNU GPL 3; see LICENSE.

jsoncpp_cflags := -Ijsoncpp/
curl_cflags := $(shell pkg-config --cflags libcurl)
curl_libs := $(shell pkg-config --libs libcurl)
ssl_cflags := $(shell pkg-config --cflags openssl)
ssl_libs := $(shell pkg-config --libs openssl)

CFLAGS = -pthread -O2 -Wall -Werror -pedantic -Wno-long-long \
         -Wno-variadic-macros -Isrc \
		 $(jsoncpp_cflags) $(curl_cflags) $(ssl_cflags)
CFLAGS_JSONCPP = -pthread -O2 -Wall $(jsoncpp_cflags)
upl_libs = -pthread $(curl_libs) $(ssl_libs)
ext_libs = $(jsoncpp_libs)
rfc_libs = $(jsoncpp_libs)

test_py_files = tests/test_uploader.py tests/test_extractor.py
headers = src/CouchDB.h src/EZ.h src/RFC3339.h \
          src/Uploader.h src/UploaderThread.h \
          src/Extractor.h src/UKHASExtractor.h \
          tests/test_extractor_mocks.h
rfc_cxxfiles = src/RFC3339.cxx tests/test_rfc3339_main.cxx
rfc_binary = tests/rfc3339
upl_cxxfiles = src/CouchDB.cxx src/EZ.cxx src/RFC3339.cxx src/Uploader.cxx
upl_thr_cflags = -DTHREADED
upl_nrm_binary = tests/cpp_connector
upl_nrm_objects = tests/test_uploader_main.o
upl_thr_binary = tests/cpp_connector_threaded
upl_thr_objects = src/UploaderThread.o tests/test_uploader_main.threaded.o
ext_cxxfiles = src/Extractor.cxx src/UKHASExtractor.cxx \
               tests/test_extractor_main.cxx
ext_binary = tests/extractor
ext_mock_cflags = -include tests/test_extractor_mocks.h

CXXFLAGS = $(CFLAGS)
CXXFLAGS_JSONCPP = $(CFLAGS_JSONCPP)
upl_objects = jsoncpp/jsoncpp.o $(patsubst %.cxx,%.o,$(upl_cxxfiles))
ext_objects = jsoncpp/jsoncpp.o $(patsubst %.cxx,%.ext_mock.o,$(ext_cxxfiles))
rfc_objects = jsoncpp/jsoncpp.o $(patsubst %.cxx,%.o,$(rfc_cxxfiles))

%.o : %.cxx $(headers)
	g++ -c $(CXXFLAGS) -o $@ $<

jsoncpp/jsoncpp.o : jsoncpp/jsoncpp.cpp jsoncpp/jsoncpp.h
	g++ -c $(CXXFLAGS_JSONCPP) -o $@ $<

%.threaded.o : %.cxx $(headers)
	g++ -c $(CXXFLAGS) $(upl_thr_cflags) -o $@ $<

%.ext_mock.o : %.cxx $(headers)
	g++ -c $(CXXFLAGS) $(ext_mock_cflags) -o $@ $<

$(upl_nrm_binary) : $(upl_objects) $(upl_nrm_objects)
	g++ $(CXXFLAGS) -o $@ $(upl_objects) $(upl_nrm_objects) $(upl_libs)

$(upl_thr_binary) : $(upl_objects) $(upl_thr_objects)
	g++ $(CXXFLAGS) -o $@ $(upl_objects) $(upl_thr_objects) $(upl_libs)

$(ext_binary) : $(ext_objects)
	g++ $(CXXFLAGS) -o $@ $(ext_objects) $(ext_libs)

$(rfc_binary) : $(rfc_objects)
	g++ $(CXXFLAGS) -o $@ $(rfc_objects) $(rfc_libs)

test : $(upl_nrm_binary) $(upl_thr_binary) $(ext_binary) $(rfc_binary) \
       $(test_py_files)
	nosetests

clean :
	rm -f $(upl_objects) $(upl_nrm_objects) $(upl_thr_objects) \
	      $(upl_nrm_binary) $(upl_thr_binary) \
		  $(ext_objects) $(ext_binary) \
	      $(patsubst %.py,%.pyc,$(test_py_files))

.PHONY : clean test
.DEFAULT_GOAL := test
