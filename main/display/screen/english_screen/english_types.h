#pragma once

#include <string>
#include <vector>

struct EnglishTask {
    std::string id;
    std::string task_date;
    std::string status;
    int phrase_count = 0;
    bool can_review = false;
};

struct EnglishEntry {
    std::string word_id;
    std::string term;
    std::string phonetic;
    std::string meaning;
    std::string phrase;
    std::string example;
};

struct EnglishReview {
    std::string label;
    std::vector<EnglishEntry> entries;
};

using EnglishTaskList = std::vector<EnglishTask>;
