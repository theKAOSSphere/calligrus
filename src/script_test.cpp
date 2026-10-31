// Script-format parser tests (console, no window).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "pdf.h"
#include "script.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

int main() {
    auto p = script::parse(
        "# c\n\nsize 1000 800\nopen a b.md\nkeys G ctrl+f ctrl+plus esc / [ alt+left\ntype hello  world\n"
        "click 5 6 right\nwheel -3\nwheel 2 10 20\nshot out/a.png\nquit\n");
    CHECK(p.error.empty());
    CHECK(p.commands.size() == 9);
    CHECK(p.commands[0].kind == script::Kind::Size && p.commands[0].a == 1000 && p.commands[0].b == 800);
    CHECK(p.commands[1].text == "a b.md");
    const auto& k = p.commands[2].keys;
    CHECK(k.size() == 7);
    CHECK(k[0].ch == U'G' && !k[0].ctrl);
    CHECK(k[1].ctrl && k[1].ch == U'f');
    CHECK(k[2].ctrl && k[2].ch == U'+');
    CHECK(k[3].named == "esc");
    CHECK(k[4].ch == U'/' && k[5].ch == U'[');
    CHECK(k[6].alt && k[6].named == "left");
    CHECK(p.commands[3].text == "hello  world");
    CHECK(p.commands[4].button == 1);
    CHECK(p.commands[5].a == -3 && !p.commands[5].has_pos);
    CHECK(p.commands[6].has_pos && p.commands[6].b == 10 && p.commands[6].c == 20);

    auto bad = script::parse("size 1 2\nfrobnicate\n");
    CHECK(bad.error.rfind("line 2:", 0) == 0);
    CHECK(!script::parse("size 1\n").error.empty());
    CHECK(!script::parse("keys ctrl+bogus+x\n").error.empty());
    CHECK(!script::parse("click 1 2 up\n").error.empty());

    // expect-tabs N ACTIVE (spec 34)
    auto et = script::parse("expect-tabs 3 2\nkeys ctrl+tab ctrl+shift+tab ctrl+pagedown ctrl+1\n");
    CHECK(et.error.empty() && et.commands.size() == 2 && et.commands[0].kind == script::Kind::ExpectTabs && et.commands[0].a == 3 && et.commands[0].b == 2);
    CHECK(et.commands[1].keys.size() == 4 && et.commands[1].keys[0].ctrl && et.commands[1].keys[0].named == "tab");
    CHECK(!script::parse("expect-tabs 2\n").error.empty() && !script::parse("expect-tabs 2 3\n").error.empty() && !script::parse("expect-tabs 0 0\n").error.empty());

    auto sd = script::parse("sync-dump out/sync.tsv\n");
    CHECK(sd.error.empty() && sd.commands.size() == 1 && sd.commands[0].kind == script::Kind::SyncDump && sd.commands[0].text == "out/sync.tsv");
    CHECK(!script::parse("sync-dump\n").error.empty());

    // Click modifiers and expect-editor (spec 23).
    auto cm = script::parse("click 5 6 ctrl\nclick 5 6 right shift alt\nmove 1 2 ctrl\nexpect-editor {\"type\":\"goto\",\"line\":3}\nexpect-editor none\n");
    CHECK(cm.error.empty() && cm.commands.size() == 5);
    CHECK(cm.commands[0].button == 0 && cm.commands[0].mod_ctrl && !cm.commands[0].mod_shift);
    CHECK(cm.commands[1].button == 1 && cm.commands[1].mod_shift && cm.commands[1].mod_alt && !cm.commands[1].mod_ctrl);
    CHECK(cm.commands[2].kind == script::Kind::Move && cm.commands[2].mod_ctrl);
    CHECK(cm.commands[3].kind == script::Kind::ExpectEditor && cm.commands[3].text == "{\"type\":\"goto\",\"line\":3}");
    CHECK(cm.commands[4].kind == script::Kind::ExpectEditor && cm.commands[4].text.empty());
    CHECK(!script::parse("click 1 2 hyper\n").error.empty());
    CHECK(!script::parse("expect-editor {nope\n").error.empty());
    CHECK(!script::parse("expect-editor\n").error.empty());

    // Slides export (spec 26): `slide N`, the command line, the PDF.
    auto sl = script::parse("slide 2\n");
    CHECK(sl.error.empty() && sl.commands[0].kind == script::Kind::Slide && sl.commands[0].a == 2);
    CHECK(!script::parse("slide 0\n").error.empty() && !script::parse("slide\n").error.empty());
    {
        std::string err;
        CHECK(!script::export_args({"talk.md"}, err) && err.empty());
        size_t n = 0;  // what the app reads back from an export child (as script.rs)
        CHECK(script::export_outcome("exported 9 slides to C:\\x\\deck.pdf\n", n, err) && n == 9);
        CHECK(script::export_outcome("exported 3 pages to C:\\x\\doc.pdf\n", n, err) && n == 3);
        CHECK(script::export_outcome("exported 1 pages to doc.pdf", n, err) && n == 1);
        CHECK(!script::export_outcome("export: cannot open a.md: no such file\n", n, err) && err == "cannot open a.md: no such file");
        CHECK(!script::export_outcome("", n, err) && err == "no answer from the export");
        err.clear();
        const script::Parsed ex = script::parse("export deck.pdf\n");
        CHECK(ex.error.empty() && ex.commands.size() == 1 && ex.commands[0].kind == script::Kind::Export && ex.commands[0].text == "deck.pdf");
        auto e = script::export_args({"--export", "talk.pdf", "talk.md"}, err);
        CHECK(e && e->input == "talk.md" && e->out == "talk.pdf" && e->width == 1280 && e->height == 720 && e->scale == 1.5f);
        e = script::export_args({"talk.md", "--size", "1024x768", "--scale", "2", "--export", "out"}, err);
        CHECK(e && e->out == "out" && e->width == 1024 && e->height == 768 && e->scale == 2.0f);
        // the pages export (spec 29): defaults, options, and the .pdf requirement
        e = script::export_args({"--export", "doc.pdf", "--pages", "doc.md"}, err);
        CHECK(e && e->pages && e->paper == "a4" && e->theme.empty() && e->scale == 2.0f && e->input == "doc.md");
        e = script::export_args({"--export", "doc.pdf", "doc.md", "--paper", "Letter", "--theme", "gruvbox", "--scale", "3", "--pages"}, err);
        CHECK(e && e->input == "doc.md" && e->paper == "letter" && e->theme == "gruvbox" && e->scale == 3.0f);
        e = script::export_args({"--export", "talk.pdf", "talk.md"}, err);
        CHECK(e && !e->pages);
        // --body-size: a number of points, 8 to 64
        e = script::export_args({"--export", "d.pdf", "d.md", "--pages"}, err);
        CHECK(e && e->body_size == 0);
        e = script::export_args({"--export", "d.pdf", "d.md", "--pages", "--body-size", "17.5"}, err);
        CHECK(e && e->body_size == 17.5f);
        e = script::export_args({"--export", "d.pdf", "d.md", "--pages", "--body-size", "8"}, err);
        CHECK(e && e->body_size == 8.0f);
        e = script::export_args({"--export", "d.pdf", "d.md", "--pages", "--body-size", "64"}, err);
        CHECK(e && e->body_size == 64.0f);
        // --base-dir: the folder relative pictures resolve from
        e = script::export_args({"--export", "d.pdf", "d.md", "--pages"}, err);
        CHECK(e && e->base_dir.empty());
        e = script::export_args({"--export", "d.pdf", "/tmp/t.md", "--base-dir", "docs/notes", "--pages"}, err);
        CHECK(e && e->base_dir == "docs/notes" && e->input == "/tmp/t.md");
        err.clear();
        CHECK(!script::export_args({"--export", "d.pdf", "d.md", "--base-dir"}, err) && !err.empty());
        // --raster (spec 33): the old one-picture pages
        e = script::export_args({"--export", "d.pdf", "d.md", "--pages"}, err);
        CHECK(e && !e->raster);
        e = script::export_args({"--export", "d.pdf", "d.md", "--pages", "--raster"}, err);
        CHECK(e && e->raster);
        e = script::export_args({"--raster", "--export", "talk.pdf", "talk.md"}, err);
        CHECK(e && e->raster);
        for (const std::vector<std::string>& wrong : std::vector<std::vector<std::string>>{
                 {"--export", "dir", "doc.md", "--pages"}, {"--export", "a.pdf", "b.md", "--pages", "--paper", "a3"}, {"--export", "a.pdf", "b.md", "--pages", "--paper"},
                 {"--export", "a.pdf", "b.md", "--pages", "--body-size", "7.5"}, {"--export", "a.pdf", "b.md", "--pages", "--body-size", "65"},
                 {"--export", "a.pdf", "b.md", "--pages", "--body-size", "big"}, {"--export", "a.pdf", "b.md", "--pages", "--body-size"}}) {
            err.clear();
            CHECK(!script::export_args(wrong, err) && !err.empty());
        }
        const script::Parsed exp = script::parse("export-pages doc.pdf\n");
        CHECK(exp.error.empty() && exp.commands.size() == 1 && exp.commands[0].kind == script::Kind::ExportPages && exp.commands[0].text == "doc.pdf");
        CHECK(!script::parse("export-pages\n").error.empty());
        for (const std::vector<std::string>& wrong : std::vector<std::vector<std::string>>{
                 {"--export", "talk.pdf"}, {"--export"}, {"--export", "a.pdf", "b.md", "--size", "10x10"}, {"--export", "a.pdf", "b.md", "--scale", "0"}}) {
            err.clear();
            CHECK(!script::export_args(wrong, err) && !err.empty());
        }
    }
    {
        // Exporting an editor's text: names of the temp file, the base folder, the dialog's file name
        namespace fs = std::filesystem;
        CHECK(script::editor_base_dir("/docs/me/notes/a.md") == fs::path("/docs/me/notes"));
        CHECK(script::editor_base_dir("a.md") == fs::path("."));
        CHECK(script::editor_base_dir("") == fs::path("."));
        CHECK(script::editor_pdf_name("/docs/me/notes/talk.org") == "talk.pdf");
        CHECK(script::editor_pdf_name("a.b.md") == "a.b.pdf");
        CHECK(script::editor_pdf_name("") == "untitled.pdf");
        const fs::path md = script::editor_temp_file(false), org = script::editor_temp_file(true), md2 = script::editor_temp_file(false);
        CHECK(md.extension() == ".md" && org.extension() == ".org" && md != md2);
        std::error_code ec;
        CHECK(md.parent_path() == (fs::temp_directory_path(ec) / "x").parent_path());  // (the temp folder may end in a slash)
    }
    {
        // GL rows come bottom up: the PDF's are top down.
        const std::vector<unsigned char> gl = {1, 2, 3, 255, 4, 5, 6, 255};  // 1 x 2: bottom row first
        CHECK((pdf::rgb_from_gl_rgba(gl, 1, 2) == std::vector<unsigned char>{4, 5, 6, 1, 2, 3}));
        pdf::Page page{4, 2, std::vector<unsigned char>(4 * 2 * 3, 10)};
        const std::string doc = pdf::write({page, page}, 0.75f);
        CHECK(doc.starts_with("%PDF-1.4\n") && doc.ends_with("%%EOF\n"));
        CHECK(doc.find("/Count 2") != std::string::npos && doc.find("/MediaBox [0 0 3.00 1.50]") != std::string::npos);
        // every xref entry points at its object
        const size_t xref = std::stoul(doc.substr(doc.rfind("startxref\n") + 10));
        CHECK(doc.compare(xref, 10, "xref\n0 9\n0") == 0);
        for (int n = 1; n <= 8; n++) {
            const size_t at = std::stoul(doc.substr(xref + 9 + 20 * size_t(n), 10));
            CHECK(doc.compare(at, std::to_string(n).size() + 6, std::to_string(n) + " 0 obj") == 0);
        }
        CHECK(pdf::zlib(page.rgb).size() > 2 && uint8_t(pdf::zlib(page.rgb)[0]) == 0x78);  // a zlib stream
    }

    std::puts(failures ? "script tests FAILED" : "script tests passed");
    return failures ? 1 : 0;
}
