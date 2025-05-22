#pragma once

#include <borealis.hpp>

struct fz_context;
struct fz_document;

class EBookView : public brls::Box {
public:
    EBookView();
    ~EBookView() override;

    void open(const std::string& input, float percent = 0);
    void render(brls::Image* view, int n);

private:
    brls::Image* left = nullptr;
    brls::Image* right = nullptr;
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
    int page = 0;
    int count = 0;
};