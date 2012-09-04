habitat cpp uploader (see habitat.uploader.Uploader)
====================================================

License
-------

habitat-cpp-uploader is GNU GPL 3 (this excludes the bundled JsonCPP,
see below). The full GNU GPL 3 text can be found in LICENSE

Building dependencies
---------------------

You will need these dependencies:

 - [libcURL](http://curl.haxx.se/)
 - habitat (for habitat.utils.rfc3339, to test the Uploader.)

You can build them from source or find install the libcurl4-openssl-dev package
on Debian based systems.

JsonCPP
-------

JsonCPP has an 'amalgamated' release where all the sources are combined into
two files, one jsoncpp.cpp and a json/json.h, which is renamed to jsoncpp.h
here to avoid conflicts with libjson's json.h.

It's in the jsoncpp/ directory of this repository. JsonCPP is Public Domain
or MIT License and full information can be found at jsoncpp/LICENSE
