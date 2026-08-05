#include <assert.h>

#include "v3_native_cache.h"

int main(void)
{
    assert(v3_native_cache_chapter_name_is_safe("TONIES_01-audio.opus"));
    assert(!v3_native_cache_chapter_name_is_safe(""));
    assert(!v3_native_cache_chapter_name_is_safe("../audio.opus"));
    assert(!v3_native_cache_chapter_name_is_safe("folder/audio.opus"));
    assert(!v3_native_cache_chapter_name_is_safe("folder\\audio.opus"));
    assert(!v3_native_cache_chapter_name_is_safe("C:audio.opus"));
    assert(!v3_native_cache_chapter_name_is_safe("audio%2fpart.opus"));
    assert(!v3_native_cache_chapter_name_is_safe("CON.opus"));
    return 0;
}
