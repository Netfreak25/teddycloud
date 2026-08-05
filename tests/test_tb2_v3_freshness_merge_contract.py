#!/usr/bin/env python3
"""Focused contracts for filtered and locally merged TB2 V3 freshness."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Tb2V3FreshnessMergeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cloud = (ROOT / "src/handler_cloud.c").read_text(encoding="utf-8")
        cls.handler = (ROOT / "src/handler.c").read_text(encoding="utf-8")
        cls.native = (ROOT / "src/v3_native_cache.c").read_text(encoding="utf-8")
        cls.docs = (ROOT / "docs/TB2_V3_CONTENT_CACHE.md").read_text(
            encoding="utf-8"
        )

    @staticmethod
    def section(source: str, start: str, end: str) -> str:
        begin = source.index(start)
        finish = source.index(end, begin + len(start))
        return source[begin:finish]

    def test_input_ruids_are_canonicalized_and_case_duplicates_are_coalesced(self):
        handler = self.section(
            self.cloud,
            "error_t handleCloudFreshnessCheckV3(",
            "error_t handleCloudCheckOtaV3(",
        )
        canonical = handler.index("tb2_ruid_canonicalize(item->string")
        numeric = handler.index("tb2_ruid_to_uid(canonical_ruid", canonical)
        duplicate = handler.index("fcInfos[previous].uid == uid", numeric)
        replace = handler.index("fcInfos[previous].audio_id =", duplicate)
        self.assertLess(canonical, numeric)
        self.assertLess(numeric, duplicate)
        self.assertLess(duplicate, replace)

    def test_private_and_nocloud_content_never_enters_cloud_map(self):
        process = self.section(
            self.cloud,
            "void process_freshness_check(",
            "static bool_t freshness_mark_content_mapping_changed_for_overlay(",
        )
        source = process.index("bool_t private_source")
        filter_call = process.index("tb2_tonie_cloud_access_allowed", source)
        add = process.index("freshness_cloud_request_add", filter_call)
        self.assertIn("forward_to_cloud = !private_source &&", process)
        self.assertLess(source, filter_call)
        self.assertLess(filter_call, add)

    def test_system_ruids_remain_separate_and_keep_box_version(self):
        process = self.section(
            self.cloud,
            "void process_freshness_check(",
            "static bool_t freshness_mark_content_mapping_changed_for_overlay(",
        )
        system = process.index("tb2_ruid_classify(canonical_ruid) == TB2_RUID_SYSTEM")
        next_content = process.index("bool_t cache_stale", system)
        branch = process[system:next_content]
        self.assertIn("freshness_cloud_request_add", branch)
        self.assertIn("freshReq->tonie_infos[i]->audio_id", branch)
        self.assertIn("continue;", branch)
        self.assertNotIn("tb2_tonie_cloud_access_allowed", branch)

    def test_only_complete_active_cache_version_replaces_box_version(self):
        evaluate = self.section(
            self.cloud,
            "static void freshness_evaluate_tonie(",
            "void process_freshness_check(",
        )
        original = evaluate.index("GENERATION_TB2 && !source_configured")
        private = evaluate.index("bool_t tap_freshness", original)
        original_branch = evaluate[original:private]
        self.assertIn("v3_native_cache_active_version", original_branch)
        self.assertIn("boxAudioIdRaw != cachedVersion", original_branch)

        active = self.section(
            self.native,
            "bool_t v3_native_cache_active_version(",
            "void v3_native_cache_invalidate(",
        )
        self.assertIn("v3_native_read_active_marker", active)
        self.assertNotIn("staging", active)

        process = self.section(
            self.cloud,
            "void process_freshness_check(",
            "static bool_t freshness_mark_content_mapping_changed_for_overlay(",
        )
        self.assertIn("decision.serverAudioIdAvailable", process)
        self.assertIn(": freshReq->tonie_infos[i]->audio_id", process)

    def test_tonies_receives_one_filtered_content_map(self):
        context = self.section(
            self.cloud,
            "typedef struct\n{\n    /* Must stay first: cloud_request reads every callback context as cbr_ctx_t. */",
            "static void v3_freshness_cloud_response(",
        )
        self.assertLess(context.index("cbr_ctx_t request;"), context.index("local_response"))

        handler = self.section(
            self.cloud,
            "error_t handleCloudFreshnessCheckV3(",
            "error_t handleCloudCheckOtaV3(",
        )
        self.assertEqual(handler.count("cloud_request_tb2_post("), 1)
        self.assertIn('cJSON_AddItemToObject(cloudReqJson, "content"', handler)
        self.assertIn("freshReqCloud.n_tonie_infos > 0", handler)
        self.assertIn("fillBaseCtx(connection, uri, queryString, V3_FRESHNESS_CHECK", handler)
        self.assertNotIn("getCloudCbr", handler)
        body = self.section(
            self.handler,
            "void cbrCloudBodyPassthrough(",
            "void cbrCloudServerDiscoPassthrough(",
        )
        self.assertNotIn("case V3_FRESHNESS_CHECK", body)

    def test_cloud_merge_is_allowlisted_and_cannot_clear_local_stale(self):
        merge = self.section(
            self.cloud,
            "static bool_t v3_freshness_merge_cloud_response(",
            "error_t handleCloudFreshnessCheckV3(",
        )
        self.assertIn("v3_freshness_cloud_requested", merge)
        self.assertIn("tb2_ruid_canonicalize", merge)
        self.assertIn("freshness_response_add_uid", merge)
        self.assertNotIn("n_tonie_marked = 0", merge)
        self.assertIn("Ignoring unexpected TONIES V3 freshness item", merge)

    def test_cloud_failures_and_incomplete_responses_keep_local_result(self):
        merge = self.section(
            self.cloud,
            "static bool_t v3_freshness_merge_cloud_response(",
            "error_t handleCloudFreshnessCheckV3(",
        )
        self.assertIn("context->status_code != 200", merge)
        self.assertIn("!context->complete", merge)
        self.assertIn("context->failed", merge)
        self.assertIn("parse_end !=", merge)

        handler = self.section(
            self.cloud,
            "error_t handleCloudFreshnessCheckV3(",
            "error_t handleCloudCheckOtaV3(",
        )
        warning = handler.index("using local decisions only")
        persist = handler.index('settings_set_u64_array_id("internal.freshnessCache"')
        response = handler.index("httpWriteResponse(connection", persist)
        self.assertLess(warning, persist)
        self.assertLess(persist, response)

    def test_documentation_defines_versions_merge_and_qos_delivery(self):
        for text in (
            "one filtered `content` map",
            "TB2 system RUIDs (`00000AF0...`)",
            "completely activated",
            "Local decisions are append-only",
            "`fresh-tonies` QoS-1 publish per RUID",
        ):
            self.assertIn(text, self.docs)


if __name__ == "__main__":
    unittest.main(verbosity=2)
