#!/usr/bin/env python3
"""Static privacy contract for the selective TB2 MQTT noCloud filter."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MqttNoCloudFilterContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.matcher = (ROOT / "src/mqtt_nocloud_filter.c").read_text(
            encoding="utf-8"
        )
        cls.header = (ROOT / "include/mqtt_nocloud_filter.h").read_text(
            encoding="utf-8"
        )
        cls.proxy = (ROOT / "src/tb2_mqtt_passthrough.c").read_text(
            encoding="utf-8"
        )
        cls.policy = (ROOT / "src/tb2_nocloud_policy.c").read_text(
            encoding="utf-8"
        )
        cls.ruid = (ROOT / "src/tb2_ruid.c").read_text(encoding="utf-8")

    def function(self, start, end):
        start_index = self.matcher.index(start)
        end_index = self.matcher.find(end, start_index + len(start))
        if end_index < 0:
            end_index = len(self.matcher)
        return self.matcher[start_index:end_index]

    def test_ruid_is_exactly_16_hex_and_case_normalized(self):
        self.assertIn("tb2_ruid_canonicalize", self.matcher)
        self.assertIn("TB2_RUID_HEX_LENGTH", self.ruid)
        self.assertIn("current - ('a' - 'A')", self.ruid)
        self.assertIn("canonical[TB2_RUID_HEX_LENGTH] = '\\0'", self.ruid)
        self.assertNotIn("mqtt_nocloud_normalize_ruid", self.matcher)

    def test_policy_lookup_is_lightweight_current_and_fail_closed(self):
        policy = self.policy
        self.assertIn("getContentPathFromCharRUID", policy)
        self.assertIn('custom_asprintf("%s.json"', policy)
        self.assertNotIn("getTonieInfo", policy)
        self.assertNotIn("load_content_json", policy)
        self.assertIn("if (!fsFileExists(json_path))", policy)
        self.assertIn("tb2_nocloud_optional_bool", policy)
        self.assertIn("policy->nocloud && !policy->cloud_override", policy)

        parser = self.policy
        self.assertIn("cJSON_ParseWithLengthOpts", parser)
        self.assertIn("parse_end != data_end", parser)
        self.assertIn("tb2_nocloud_json_space", parser)

    def test_policy_uses_effective_overlay_without_new_source_lifecycle(self):
        self.assertIn(
            "policy->overlay_id = settings->internal.overlayNumber", self.policy
        )
        self.assertIn("TB2_RUID_SYSTEM", self.policy)
        self.assertIn("policy->kind == TB2_RUID_CONTENT", self.policy)
        self.assertNotIn("nocloud_manual", self.policy)
        self.assertNotIn("nocloud_source", self.policy)

    def test_unique_ruids_are_cached_only_for_one_packet(self):
        lookup = self.function(
            "static bool_t mqtt_nocloud_is_protected",
            "static void mqtt_nocloud_free_context",
        )
        self.assertIn("context->policies", lookup)
        self.assertIn("osStrcmp(entry->ruid, normalized)", lookup)
        self.assertIn("tb2_nocloud_policy_resolve", lookup)
        self.assertIn("entry->resolved", lookup)
        dispatcher = self.function(
            "void mqtt_nocloud_filter_publish",
            "void mqtt_nocloud_filter_result_free",
        )
        self.assertIn("mqtt_nocloud_context_t context", dispatcher)
        self.assertIn("mqtt_nocloud_free_context(&context)", dispatcher)
        self.assertNotIn("static mqtt_nocloud_context_t", self.matcher)

    def test_box_to_cloud_sensitive_topics_are_explicit(self):
        dispatcher = self.function(
            "void mqtt_nocloud_filter_publish",
            "void mqtt_nocloud_filter_result_free",
        )
        for path in (
            '"claim/"',
            '"playback/state"',
            '"metrics/fleet"',
            '"metrics/events"',
            '"bi-events"',
            '"logs"',
        ):
            self.assertIn(path, dispatcher)
        self.assertIn("box_to_upstream &&", dispatcher)

    def test_cloud_to_box_filter_is_limited_to_fresh_tonies(self):
        dispatcher = self.function(
            "void mqtt_nocloud_filter_publish",
            "void mqtt_nocloud_filter_result_free",
        )
        self.assertIn(
            '!box_to_upstream && osStrcmp(path, "fresh-tonies") == 0',
            dispatcher,
        )
        local_publish = self.proxy[
            self.proxy.index("tb2_mqtt_passthrough_write_local_publish") :
            self.proxy.index("void tb2_mqtt_passthrough_close")
        ]
        self.assertNotIn("mqtt_nocloud_filter_publish", local_publish)

    def test_teddycloud_payload_is_blocked_before_topic_or_policy_lookup(self):
        dispatcher = self.function(
            "void mqtt_nocloud_filter_publish",
            "void mqtt_nocloud_filter_result_free",
        )
        local_guard = dispatcher.index('"teddycloud_"')
        topic_lookup = dispatcher.index("mqtt_nocloud_topic_path", local_guard)
        self.assertLess(local_guard, topic_lookup)
        self.assertIn("box_to_upstream &&", dispatcher[:topic_lookup])
        self.assertIn('"local_content.teddycloud_payload"', dispatcher)

    def test_arrays_remove_whole_protected_items_and_keep_other_data(self):
        array_filter = self.function(
            "static void mqtt_nocloud_filter_array",
            "static void mqtt_nocloud_filter_playback",
        )
        self.assertIn("cJSON_IsArray(json)", array_filter)
        self.assertIn("mqtt_nocloud_item_is_protected", array_filter)
        self.assertIn("cJSON_DeleteItemFromArray(json, index)", array_filter)
        self.assertIn("removed++", array_filter)
        self.assertIn("mqtt_nocloud_finish_array", array_filter)
        finish = self.function(
            "static void mqtt_nocloud_finish_array",
            "static bool_t mqtt_nocloud_item_is_protected",
        )
        self.assertIn("if (removed == 0)", finish)
        self.assertIn("cJSON_GetArraySize(json) == 0", finish)
        self.assertIn("cJSON_PrintUnformatted", finish)
        # Whole-object deletion covers metric 1000/1001 and all associated
        # chapter, duration, play-time, content-version and end-reason fields.
        self.assertNotIn("cJSON_DeleteItemFromObject", array_filter)

    def test_playback_null_is_forwarded_and_protected_ruid_is_blocked(self):
        playback = self.function(
            "static void mqtt_nocloud_filter_playback",
            "static void mqtt_nocloud_filter_fresh_tonies",
        )
        self.assertIn("cJSON_IsNull(tonie)", playback)
        self.assertIn("mqtt_nocloud_is_protected", playback)
        self.assertIn('mqtt_nocloud_block(result, filter_id, 1)', playback)

    def test_fresh_tonies_accepts_object_string_array_and_object_array(self):
        fresh = self.function(
            "static void mqtt_nocloud_filter_fresh_tonies",
            "static void mqtt_nocloud_filter_logs",
        )
        self.assertIn("cJSON_IsArray(json)", fresh)
        self.assertIn("mqtt_nocloud_filter_array", fresh)
        self.assertIn("cJSON_IsObject(json)", fresh)
        item = self.function(
            "static bool_t mqtt_nocloud_item_is_protected",
            "static void mqtt_nocloud_filter_array",
        )
        self.assertIn("cJSON_IsObject(item)", item)
        self.assertIn('cJSON_GetObjectItemCaseSensitive(item, "tonie")', item)
        self.assertIn("cJSON_IsString(tonie)", item)

    def test_logs_require_a_bounded_16_hex_token(self):
        logs = self.function(
            "static void mqtt_nocloud_filter_logs",
            "void mqtt_nocloud_filter_publish",
        )
        self.assertIn("end - index == TB2_RUID_HEX_LENGTH", logs)
        self.assertIn("index > 0 && isxdigit(payload[index - 1])", logs)
        self.assertIn("while (end < payload_len && isxdigit", logs)
        self.assertIn("mqtt_nocloud_is_protected", logs)

    def test_log_arrays_remove_only_protected_entries(self):
        logs = self.function(
            "static void mqtt_nocloud_filter_logs",
            "void mqtt_nocloud_filter_publish",
        )
        self.assertIn("cJSON_IsArray(json)", logs)
        self.assertIn("cJSON_PrintUnformatted(item)", logs)
        self.assertIn("cJSON_DeleteItemFromArray(json, index)", logs)
        self.assertIn("mqtt_nocloud_finish_array", logs)
        self.assertIn("mqtt_nocloud_payload_has_protected_ruid", logs)

    def test_invalid_structured_payloads_fail_closed_without_session_error(self):
        for function_name in (
            "mqtt_nocloud_filter_array",
            "mqtt_nocloud_filter_playback",
            "mqtt_nocloud_filter_fresh_tonies",
        ):
            self.assertIn(function_name, self.matcher)
        self.assertIn("mqtt_nocloud_block(result, filter_id, 0)", self.matcher)
        processor = self.proxy[
            self.proxy.index("static error_t tb2_mqtt_process_packet") :
            self.proxy.index("static error_t tb2_mqtt_process_stream")
        ]
        self.assertIn("rebuild_error", processor)
        self.assertIn("nocloud_result.action = MQTT_NOCLOUD_BLOCK", processor)
        self.assertIn("tb2_mqtt_send_generated_ack", processor)


if __name__ == "__main__":
    unittest.main(verbosity=2)
