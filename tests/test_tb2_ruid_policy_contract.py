#!/usr/bin/env python3
"""Focused source contract for the shared TB2 rUID and NoCloud policy."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2RuidPolicyContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ruid_h = (ROOT / "include/tb2_ruid.h").read_text(encoding="utf-8")
        cls.ruid_c = (ROOT / "src/tb2_ruid.c").read_text(encoding="utf-8")
        cls.policy = (ROOT / "src/tb2_nocloud_policy.c").read_text(
            encoding="utf-8"
        )
        cls.matcher = (ROOT / "src/mqtt_nocloud_filter.c").read_text(
            encoding="utf-8"
        )
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.v3 = (ROOT / "src/v3_local_content.c").read_text(encoding="utf-8")
        cls.state = (ROOT / "src/toniebox_state.c").read_text(encoding="utf-8")
        cls.identity = (ROOT / "src/tb2_client_identity.c").read_text(
            encoding="utf-8"
        )
        cls.request = (ROOT / "src/cloud_request.c").read_text(encoding="utf-8")
        cls.passthrough = (ROOT / "src/tb2_https_passthrough.c").read_text(
            encoding="utf-8"
        )

    def test_one_canonical_uppercase_representation_is_shared(self):
        self.assertIn("TB2_RUID_HEX_LENGTH 16U", self.ruid_h)
        self.assertIn("tb2_ruid_canonicalize", self.matcher)
        self.assertIn("tb2_ruid_canonicalize", self.v3)
        self.assertIn("tb2_ruid_canonicalize", self.state)
        self.assertIn("tb2_ruid_to_uid", self.cloud)
        self.assertIn("tb2_ruid_from_uid", self.cloud)
        self.assertNotIn("mqtt_nocloud_normalize_ruid", self.matcher)
        self.assertNotIn("v3_local_normalize_ruid", self.v3)
        self.assertNotIn("freshness_ruid_to_uid", self.cloud)

    def test_system_ruids_are_explicit_and_not_content_policy(self):
        self.assertIn('#define TB2_SYSTEM_RUID_PREFIX "00000AF0"', self.ruid_h)
        self.assertIn("TB2_RUID_SYSTEM", self.ruid_c)
        system_guard = self.policy.index("if (policy->kind == TB2_RUID_SYSTEM)")
        content_lookup = self.policy.index("getContentPathFromCharRUID")
        self.assertLess(system_guard, content_lookup)
        self.assertIn("policy->kind == TB2_RUID_CONTENT", self.policy)
        self.assertIn("tb2_ruid_classify(checked_ruid) == TB2_RUID_CONTENT", self.cloud)
        self.assertIn("freshness_cloud_request_add(freshReqCloud", self.cloud)
        self.assertIn("tb2_ruid_classify(ruid) == TB2_RUID_SYSTEM", self.cloud)

    def test_nocloud_is_overlay_ruid_based_and_not_model_based(self):
        self.assertIn("policy->overlay_id = settings->internal.overlayNumber", self.policy)
        self.assertIn("getContentPathFromCharRUID(policy->ruid", self.policy)
        self.assertNotIn("tonie_model", self.policy)
        self.assertIn("tb2_nocloud_policy_from_content", self.cloud)
        self.assertIn("tb2_nocloud_policy_resolve", self.matcher)

    def test_legacy_persisted_case_and_missing_boole_are_compatible(self):
        self.assertIn("item == NULL", self.policy)
        self.assertIn("*value = FALSE", self.policy)
        self.assertIn("tb2_ruid_canonicalize(ruid_json->valuestring", self.v3)
        self.assertIn("osStrcasecmp(expected_name, chapter->name)", self.v3)

    def test_cache_identity_binds_overlay_ruid_version_and_chapters(self):
        self.assertIn("overlay_id", self.v3)
        self.assertIn("canonical_ruid", self.v3)
        self.assertIn("effective_version", self.v3)
        self.assertIn('cJSON_AddStringToObject(root, "ruid", generation->ruid)', self.v3)
        self.assertIn('cJSON_AddNumberToObject(root, "effectiveVersion"', self.v3)
        self.assertIn('cJSON_AddStringToObject(entry, "name", chapter->name)', self.v3)

    def test_tb2_certificate_and_content_auth_selection_are_central(self):
        self.assertIn("tb2_client_identity_resolve", self.request)
        self.assertIn("tb2_client_identity_resolve", self.passthrough)
        self.assertIn("tb2_content_identity_resolve", self.cloud)
        self.assertIn("tb2_ruid_canonicalize(selected_ruid", self.identity)
        self.assertIn("content->cloud_auth_len != TONIE_AUTH_TOKEN_LENGTH", self.identity)

    def test_tb1_cloud_policy_remains_on_legacy_branch(self):
        self.assertIn("static bool_t tonie_cloud_access_allowed", self.cloud)
        self.assertIn(
            "allow_cloud_override && settings->toniebox.boxGeneration == GENERATION_TB2",
            self.cloud,
        )
        self.assertIn("(!tonieInfo->json.nocloud ||", self.cloud)
        self.assertIn("tonieInfo->json.cloud_override", self.cloud)

    def test_v3_freshness_rejects_invalid_versions_before_storage(self):
        self.assertIn("item->valuedouble < 0", self.cloud)
        self.assertIn("item->valuedouble > UINT32_MAX", self.cloud)
        self.assertIn("(double)(uint32_t)item->valuedouble != item->valuedouble", self.cloud)


if __name__ == "__main__":
    unittest.main(verbosity=2)
