#include "preview_frame.h"
#include <cstdio>
#define CHECK(x) do {if(!(x)){std::printf("line %d: %s\n",__LINE__,#x);return 1;}} while(false)
int main() {
    grid2vr::PreviewFrame preview;
    CHECK(!preview.Consume(1,true).scene);
    preview.Publish(12,300,true,true);
    const auto first=preview.Consume(12,true);
    CHECK(first.scene==300 && first.audit);
    CHECK(!preview.Consume(12,true).scene);
    preview.Publish(13,301,true,false);
    CHECK(!preview.Consume(14,true).scene);
    CHECK(!preview.Consume(13,true).scene);
    preview.Publish(14,302,true,true);
    CHECK(!preview.Consume(14,false).scene);
    CHECK(!preview.Consume(14,true).scene);
    preview.Publish(15,303,true,true);
    preview.Publish(15,304,false,true);
    CHECK(!preview.Consume(15,true).scene);
    preview.Publish(16,305,true,true);preview.Reset();
    CHECK(!preview.Consume(16,true).scene);
    preview.Publish(17,0,true,true);
    CHECK(!preview.Consume(17,true).scene);
    preview.Publish(18,306,true,false);
    const auto last=preview.Consume(18,true);
    CHECK(last.scene==306 && !last.audit);
    return 0;
}
