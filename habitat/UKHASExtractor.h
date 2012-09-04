/* Copyright 2011 (C) Daniel Richman. License: GNU GPL 3; see COPYING. */

#ifndef HABITAT_UKHAS_EXTRACTOR_H
#define HABITAT_UKHAS_EXTRACTOR_H

#include "habitat/Extractor.h"

namespace habitat {

class UKHASExtractor : public Extractor
{
    int extracting;
    char last;
    string buffer;
    int skipped_count;
    int garbage_count;

    void reset_buffer();
    Json::Value crude_parse();

public:
    UKHASExtractor() : extracting(false), last('\0'), garbage_count(0) {};
    ~UKHASExtractor() {};
    void skipped(int n);
    void push(char b, enum push_flags flags);
};

} /* namespace habitat */

#endif /* HABITAT_UKHAS_EXTRACTOR_H */
