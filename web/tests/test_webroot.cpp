#include "../APC_WebRoot.h"
#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    char directory[] = "/tmp/apcam-webroot-XXXXXX";
    assert(mkdtemp(directory));
    APC_WebRoot root;
    root.set_path(directory);
    const std::string path = std::string(directory) + "/head.html";
    const auto write = [&](const char *text) {
        FILE *file = fopen(path.c_str(), "w");
        assert(file);
        assert(fputs(text, file) >= 0);
        assert(fclose(file) == 0);
    };
    APC_StringBuffer body;
    write("<p>{{0}} {{1}}</p>");
    assert(root.render(body, "head.html", {"translated &amp; escaped", APC_TemplateValue("%.1f", 1.25)}));
    assert(strcmp(body.data(), "<p>translated &amp; escaped 1.2</p>") == 0);
    body.reset();
    assert(!root.append(body, "../head.html") && !body.valid());
    body.reset();
    assert(!root.append(body, "/etc/passwd") && !body.valid());
    body.reset();
    write("{{999}}");
    assert(!root.render(body, "head.html", {"value"}));
    assert(body.release() == nullptr);
    unlink(path.c_str());
    assert(symlink("/etc/passwd", path.c_str()) == 0);
    assert(!root.append(body, "head.html") && !body.valid());
    body.reset();
    unlink(path.c_str());
    assert(mkfifo(path.c_str(), 0600) == 0);
    assert(!root.append(body, "head.html")); // Must not block on a special file.
    body.reset();
    unlink(path.c_str());
    int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && ftruncate(fd, 256U * 1024U + 1U) == 0);
    close(fd);
    assert(!root.append(body, "head.html"));
    body.reset();
    unlink(path.c_str());
    assert(!root.append(body, "head.html"));
    // Only the pinned TS player vendor asset may exceed the normal limit.
    const std::string vendor = std::string(directory) + "/mpegts.js";
    fd = open(vendor.c_str(), O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && ftruncate(fd, 300U * 1024U) == 0);
    close(fd);
    body.reset();
    assert(root.append(body, "mpegts.js") && body.size() == 300U * 1024U);
    fd = open(vendor.c_str(), O_WRONLY);
    assert(fd >= 0 && ftruncate(fd, 512U * 1024U + 1U) == 0);
    close(fd);
    body.reset();
    assert(!root.append(body, "mpegts.js"));
    unlink(vendor.c_str());
    rmdir(directory);
    return 0;
}
