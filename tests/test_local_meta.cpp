#include <cstdio>
#include <cstring>

#include "fm1/platform/library.hpp"

int main() {
  const char* path = "/tmp/fm1-localmeta-test.fwsc";
  const char data[128]{};
  std::FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return 1;
  std::fwrite(data, 1, sizeof(data), file);
  std::fclose(file);

  fm1::catalog::Entry entry;
  entry.id = "demo@1.2";
  entry.name = "Demo";
  entry.version = "1.2";
  entry.author = "Ada";
  entry.what = "desc";
  entry.tags = "synth";
  entry.product = "FM-1_900";

  auto written = fm1::platform::write_local_metadata(path, "local:demo", "abc", sizeof(data),
                                                     &entry);
  if (!written) return 2;
  auto meta = fm1::platform::read_local_metadata(path);
  if (!meta.valid || meta.name != "Demo" || meta.author != "Ada" ||
      meta.version != "1.2" || meta.product != "FM-1_900" || meta.size != sizeof(data)) {
    return 3;
  }
  std::remove(path);
  std::remove("/tmp/fm1-localmeta-test.fwsc.meta");
  return 0;
}
