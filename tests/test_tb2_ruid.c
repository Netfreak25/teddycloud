#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "tb2_ruid.h"

int main(void)
{
    char canonical[TB2_RUID_SIZE];
    uint64_t lower_uid = 0;
    uint64_t upper_uid = 0;

    assert(tb2_ruid_canonicalize("77f73e23500304e0", canonical));
    assert(strcmp(canonical, "77F73E23500304E0") == 0);
    assert(tb2_ruid_canonicalize("77F73E23500304E0", canonical));
    assert(strcmp(canonical, "77F73E23500304E0") == 0);
    assert(!tb2_ruid_canonicalize("77F73E23500304E", canonical));
    assert(!tb2_ruid_canonicalize("77F73E23500304EG", canonical));

    assert(tb2_ruid_to_uid("77f73e23500304e0", &lower_uid));
    assert(tb2_ruid_to_uid("77F73E23500304E0", &upper_uid));
    assert(lower_uid == upper_uid);
    tb2_ruid_from_uid(lower_uid, canonical);
    assert(strcmp(canonical, "77F73E23500304E0") == 0);

    assert(tb2_ruid_classify("77f73e23500304e0") == TB2_RUID_CONTENT);
    assert(tb2_ruid_classify("00000af000000003") == TB2_RUID_SYSTEM);
    assert(tb2_ruid_classify("00000AF0FFFFFFFF") == TB2_RUID_SYSTEM);
    assert(tb2_ruid_classify("invalid") == TB2_RUID_INVALID);
    return 0;
}
