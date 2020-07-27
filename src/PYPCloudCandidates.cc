/* vim:set et ts=4 sts=4:
 *
 * ibus-libpinyin - Intelligent Pinyin engine based on libpinyin for IBus
 *
 * Copyright (c) 2018 linyu Xu <liannaxu07@gmail.com>
 * Copyright (c) 2020 Weixuan XIAO <veyx.shaw@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "PYPCloudCandidates.h"
#include "PYString.h"
#include "PYConfig.h"
#include "PYPPhoneticEditor.h"
#include "PYPPinyinEditor.h"

#include <assert.h>
#include <pinyin.h>
#include <cstring>
#include <glib.h>


using namespace PY;

enum CandidateResponseParserError {
    PARSER_NOERR,
    PARSER_INVALID_DATA,
    PARSER_BAD_FORMAT,
    PARSER_NO_CANDIDATE,
    PARSER_NETWORK_ERROR,
    PARSER_UNKNOWN
};

static const std::string CANDIDATE_CLOUD_PREFIX = "☁";

static const std::string CANDIDATE_PENDING_TEXT_WITHOUT_PREFIX      = "[⏱️]";
static const std::string CANDIDATE_LOADING_TEXT_WITHOUT_PREFIX      = "...";
static const std::string CANDIDATE_NO_CANDIDATE_TEXT_WITHOUT_PREFIX = "[🚫]";
static const std::string CANDIDATE_INVALID_DATA_TEXT_WITHOUT_PREFIX = "[❌]";
static const std::string CANDIDATE_BAD_FORMAT_TEXT_WITHOUT_PREFIX   = "[❓]";

static const std::string CANDIDATE_PENDING_TEXT         = CANDIDATE_CLOUD_PREFIX + CANDIDATE_PENDING_TEXT_WITHOUT_PREFIX;
static const std::string CANDIDATE_LOADING_TEXT         = CANDIDATE_CLOUD_PREFIX + CANDIDATE_LOADING_TEXT_WITHOUT_PREFIX;
static const std::string CANDIDATE_NO_CANDIDATE_TEXT    = CANDIDATE_CLOUD_PREFIX + CANDIDATE_NO_CANDIDATE_TEXT_WITHOUT_PREFIX;
static const std::string CANDIDATE_INVALID_DATA_TEXT    = CANDIDATE_CLOUD_PREFIX + CANDIDATE_INVALID_DATA_TEXT_WITHOUT_PREFIX ;
static const std::string CANDIDATE_BAD_FORMAT_TEXT      = CANDIDATE_CLOUD_PREFIX + CANDIDATE_BAD_FORMAT_TEXT_WITHOUT_PREFIX;

const char *BAIDU_URL_TEMPLATE = "http://olime.baidu.com/py?input=%s&inputtype=py&bg=0&ed=%d&result=hanzi&resultcoding=utf-8&ch_en=1&clientinfo=web&version=1";
const char *GOOGLE_URL_TEMPLATE = "https://www.google.com/inputtools/request?ime=pinyin&text=%s&num=%d";

typedef struct
{
    guint event_id;
    const gchar request_str[MAX_PINYIN_LEN + 1];
    CloudCandidates *cloud_candidates;
} DelayedCloudAsyncRequestCallbackUserData;

class CloudCandidatesResponseParser
{
public:
    CloudCandidatesResponseParser () : m_annotation (NULL) {}
    virtual ~CloudCandidatesResponseParser () {}

    virtual guint parse (GInputStream *stream) = 0;
    virtual guint parse (const gchar *data) = 0;

    virtual std::vector<std::string> &getStringCandidates () { return m_candidates; }
    virtual std::vector<EnhancedCandidate> getCandidates ();
    virtual const gchar *getAnnotation () { return m_annotation; }

protected:
    std::vector<std::string> m_candidates;
    const gchar *m_annotation;
};

class CloudCandidatesResponseJsonParser : public CloudCandidatesResponseParser
{
public:
    CloudCandidatesResponseJsonParser ();
    virtual ~CloudCandidatesResponseJsonParser ();

    guint parse (GInputStream *stream)
    {
        GError **error = NULL;

        if (!stream)
            return PARSER_NETWORK_ERROR;

        /* parse Json from input steam */
        if (!json_parser_load_from_stream (m_parser, stream, NULL, error) || error != NULL) {
            g_input_stream_close (stream, NULL, error);  /* Close stream to release libsoup connexion */
            return PARSER_BAD_FORMAT;
        }
        g_input_stream_close (stream, NULL, error);  /* Close stream to release libsoup connexion */

        return parseJsonResponse (json_parser_get_root (m_parser));
    }

    guint parse (const gchar *data)
    {
        GError **error = NULL;

        if (!data)
            return PARSER_NETWORK_ERROR;

        /* parse Json from data */
        if (!json_parser_load_from_data (m_parser, data, strlen (data), error) || error != NULL)
            return PARSER_BAD_FORMAT;

        return parseJsonResponse (json_parser_get_root (m_parser));
    }

protected:
    JsonParser *m_parser;

    virtual guint parseJsonResponse (JsonNode *root) = 0;
};

class GoogleCloudCandidatesResponseJsonParser : public CloudCandidatesResponseJsonParser
{
protected:
    guint parseJsonResponse (JsonNode *root)
    {
        if (!JSON_NODE_HOLDS_ARRAY (root))
            return PARSER_BAD_FORMAT;

        /* validate Google source and the structure of response */
        JsonArray *google_root_array = json_node_get_array (root);

        const gchar *google_response_status;
        JsonArray *google_response_array;
        JsonArray *google_result_array;
        const gchar *google_candidate_annotation;
        JsonArray *google_candidate_array;
        guint result_counter;

        if (json_array_get_length (google_root_array) <= 1)
            return PARSER_INVALID_DATA;

        google_response_status = json_array_get_string_element (google_root_array, 0);

        if (g_strcmp0 (google_response_status, "SUCCESS"))
            return PARSER_INVALID_DATA;

        google_response_array = json_array_get_array_element (google_root_array, 1);

        if (json_array_get_length (google_response_array) < 1)
            return PARSER_INVALID_DATA;

        google_result_array = json_array_get_array_element (google_response_array, 0);

        google_candidate_annotation = json_array_get_string_element (google_result_array, 0);

        if (!google_candidate_annotation)
            return PARSER_INVALID_DATA;

        /* update annotation with the returned annotation */
        m_annotation = google_candidate_annotation;

        google_candidate_array = json_array_get_array_element (google_result_array, 1);

        result_counter = json_array_get_length (google_candidate_array);

        if (result_counter < 1)
            return PARSER_NO_CANDIDATE;

        for (guint i = 0; i < result_counter; ++i) {
            std::string candidate = json_array_get_string_element (google_candidate_array, i);
            m_candidates.push_back (candidate);
        }

        return PARSER_NOERR;
    }

public:
    GoogleCloudCandidatesResponseJsonParser () : CloudCandidatesResponseJsonParser () {}
};

class BaiduCloudCandidatesResponseJsonParser : public CloudCandidatesResponseJsonParser
{
private:
    guint parseJsonResponse (JsonNode *root)
    {
        if (!JSON_NODE_HOLDS_OBJECT (root))
            return PARSER_BAD_FORMAT;

        /* validate Baidu source and the structure of response */
        JsonObject *baidu_root_object = json_node_get_object (root);
        const gchar *baidu_response_status;
        JsonArray *baidu_result_array;
        JsonArray *baidu_candidate_array;
        const gchar *baidu_candidate_annotation;
        guint result_counter;

        if (!json_object_has_member (baidu_root_object, "status"))
            return PARSER_INVALID_DATA;

        baidu_response_status = json_object_get_string_member (baidu_root_object, "status");

        if (g_strcmp0 (baidu_response_status, "T"))
            return PARSER_INVALID_DATA;

        if (!json_object_has_member (baidu_root_object, "result"))
            return PARSER_INVALID_DATA;

        baidu_result_array = json_object_get_array_member (baidu_root_object, "result");

        baidu_candidate_array = json_array_get_array_element (baidu_result_array, 0);
        baidu_candidate_annotation = json_array_get_string_element (baidu_result_array, 1);

        if (!baidu_candidate_annotation)
            return PARSER_INVALID_DATA;

        /* update annotation with the returned annotation */
        m_annotation = NULL;
        gchar **words = g_strsplit (baidu_candidate_annotation, "'", -1);
        m_annotation = g_strjoinv ("", words);
        g_strfreev (words);

        result_counter = json_array_get_length (baidu_candidate_array);

        if (result_counter < 1)
            return PARSER_NO_CANDIDATE;

        for (guint i = 0; i < result_counter; ++i) {
            std::string candidate;
            JsonArray *baidu_candidate = json_array_get_array_element (baidu_candidate_array, i);

            if (json_array_get_length (baidu_candidate) < 1)
                candidate = CANDIDATE_INVALID_DATA_TEXT_WITHOUT_PREFIX;
            else
                candidate = json_array_get_string_element (baidu_candidate, 0);

            m_candidates.push_back (candidate);
        }

        return PARSER_NOERR;
    }

public:
    BaiduCloudCandidatesResponseJsonParser () : CloudCandidatesResponseJsonParser () {}
    ~BaiduCloudCandidatesResponseJsonParser () { if (m_annotation) g_free ((gpointer)m_annotation); }
};

gboolean
CloudCandidates::delayedCloudAsyncRequestCallBack (gpointer user_data)
{
    DelayedCloudAsyncRequestCallbackUserData *data = static_cast<DelayedCloudAsyncRequestCallbackUserData *> (user_data);
    CloudCandidates *cloudCandidates;

    if (!data)
        return FALSE;

    cloudCandidates = data->cloud_candidates;

    if (!cloudCandidates)
        return FALSE;

    /* only send with a latest timer */
    if (data->event_id == cloudCandidates->m_source_event_id) {
        cloudCandidates->m_source_event_id = 0;
        cloudCandidates->cloudAsyncRequest (data->request_str);
    }

    return FALSE;
}

void
CloudCandidates::delayedCloudAsyncRequestDestroyCallBack (gpointer user_data)
{
    /* clean up */
    if (user_data)
        g_free (user_data);
}

CloudCandidates::CloudCandidates (PhoneticEditor * editor)
{
    m_session = soup_session_new ();
    m_editor = editor;

    m_cloud_source = m_editor->m_config.cloudInputSource ();
    m_delayed_time = m_editor->m_config.cloudRequestDelayTime ();
    m_cloud_candidates_number = m_editor->m_config.cloudCandidatesNumber ();

    m_source_event_id = 0;
    m_message = NULL;

    m_last_requested_pinyin = (gchar *) g_malloc (sizeof(gchar) * (MAX_PINYIN_LEN + 1));
    m_last_requested_pinyin[0] = '\0';
}

CloudCandidates::~CloudCandidates ()
{
    g_free (m_last_requested_pinyin);
}

gboolean
CloudCandidates::processCandidates (std::vector<EnhancedCandidate> & candidates)
{
    /* refer pinyin retrieved in full pinyin mode */
    const gchar *full_pinyin_text;

    /* refer pinyin generated in double pinyin mode, need free */
    gchar *double_pinyin_text;

    /*  */
    std::vector<EnhancedCandidate>::iterator cloud_candidates_first_pos;

    /* check the length of the first n-gram candidate */
    std::vector<EnhancedCandidate>::iterator n_gram_sentence_candidate = candidates.begin ();
    if (n_gram_sentence_candidate == candidates.end ()) {
        return FALSE;   /* no candidate */
    }
    if (g_utf8_strlen (n_gram_sentence_candidate->m_display_string.c_str (), -1) < CLOUD_MINIMUM_UTF8_TRIGGER_LENGTH) {
        m_last_requested_pinyin[0] = '\0';
        return FALSE;   /* do not request because there is only one character */
    }

    /* search the first non-ngram candidate */
    for (cloud_candidates_first_pos = candidates.begin (); cloud_candidates_first_pos != candidates.end (); ++cloud_candidates_first_pos) {
        if (CANDIDATE_NBEST_MATCH != cloud_candidates_first_pos->m_candidate_type)
            break;
    }

    /* check pinyin length */
    if (! m_editor->m_config.doublePinyin ()) {
        full_pinyin_text = m_editor->m_text;

        if (strcmp (m_last_requested_pinyin, full_pinyin_text) == 0) {
            /* do not request again and update cached ones */
            std::vector<EnhancedCandidate> m_candidates_with_prefix;
            for (std::vector<EnhancedCandidate>::iterator i = m_candidates.begin (); i != m_candidates.end (); ++i) {
                EnhancedCandidate candidate_with_prefix = *i;
                candidate_with_prefix.m_display_string = CANDIDATE_CLOUD_PREFIX + candidate_with_prefix.m_display_string;
                m_candidates_with_prefix.push_back (candidate_with_prefix);
            }
            candidates.insert (cloud_candidates_first_pos, m_candidates_with_prefix.begin (), m_candidates_with_prefix.end ());
            return FALSE;
        }
    }
    else
    {
        m_editor->updateAuxiliaryText ();
        String stripped = m_editor->m_buffer;
        const gchar *temp= stripped;
        gchar** tempArray =  g_strsplit_set (temp, " |", -1);
        double_pinyin_text = g_strjoinv ("", tempArray);

        g_strfreev (tempArray);

        if (strcmp (m_last_requested_pinyin, double_pinyin_text) == 0) {
            /* do not request again and update cached one */
            std::vector<EnhancedCandidate> m_candidates_with_prefix;
            for (std::vector<EnhancedCandidate>::iterator i = m_candidates.begin (); i != m_candidates.end (); ++i) {
                EnhancedCandidate candidate_with_prefix = *i;
                candidate_with_prefix.m_display_string = CANDIDATE_CLOUD_PREFIX + candidate_with_prefix.m_display_string;
                m_candidates_with_prefix.push_back (candidate_with_prefix);
            }
            candidates.insert (cloud_candidates_first_pos, m_candidates_with_prefix.begin (), m_candidates_with_prefix.end ());

            g_free (double_pinyin_text);
            return FALSE;
        }
    }

    /* have cloud candidates already */
    EnhancedCandidate testCan = *cloud_candidates_first_pos;
    if (testCan.m_candidate_type == CANDIDATE_CLOUD_INPUT)
        return FALSE;

    /* insert cloud candidates' placeholders */
    m_candidates.clear ();
    for (guint i = 0; i < m_cloud_candidates_number; ++i) {
        EnhancedCandidate enhanced;
        enhanced.m_display_string = CANDIDATE_PENDING_TEXT;
        enhanced.m_candidate_type = CANDIDATE_CLOUD_INPUT;
        m_candidates.push_back (enhanced);
    }
    candidates.insert (cloud_candidates_first_pos, m_candidates.begin (), m_candidates.end ());

    /* update configuration before request */
    m_cloud_source = m_editor->m_config.cloudInputSource ();
    m_delayed_time = m_editor->m_config.cloudRequestDelayTime ();
    m_cloud_candidates_number = m_editor->m_config.cloudCandidatesNumber ();
    if (! m_editor->m_config.doublePinyin ()) {
        delayedCloudAsyncRequest (full_pinyin_text);
    }
    else {
        delayedCloudAsyncRequest (double_pinyin_text);
        g_free (double_pinyin_text);
    }

    return TRUE;
}

int
CloudCandidates::selectCandidate (EnhancedCandidate & enhanced)
{
    assert (CANDIDATE_CLOUD_INPUT == enhanced.m_candidate_type);

    if (enhanced.m_display_string == CANDIDATE_PENDING_TEXT ||
        enhanced.m_display_string == CANDIDATE_LOADING_TEXT ||
        enhanced.m_display_string == CANDIDATE_BAD_FORMAT_TEXT ||
        enhanced.m_display_string == CANDIDATE_INVALID_DATA_TEXT)
        return SELECT_CANDIDATE_ALREADY_HANDLED;

    /* take the cached candidate with the same candidate id */
    for (std::vector<EnhancedCandidate>::iterator pos = m_candidates.begin (); pos != m_candidates.end (); ++pos) {
        if (pos->m_candidate_id == enhanced.m_candidate_id) {
            enhanced.m_display_string = pos->m_display_string;

            /* modify in-place and commit */
            return SELECT_CANDIDATE_COMMIT | SELECT_CANDIDATE_MODIFY_IN_PLACE;
        }
    }

    return SELECT_CANDIDATE_ALREADY_HANDLED;
}

void
CloudCandidates::delayedCloudAsyncRequest (const gchar* requestStr)
{
    gpointer user_data;
    DelayedCloudAsyncRequestCallbackUserData *data;
    guint event_id;

    /* cancel the latest timer, if applied */
    if (m_source_event_id != 0)
        g_source_remove (m_source_event_id);

    /* allocate memory for a DelayedCloudAsyncRequestCallbackUserData instance to take more callback user data */
    user_data = g_malloc (sizeof(DelayedCloudAsyncRequestCallbackUserData));
    data = static_cast<DelayedCloudAsyncRequestCallbackUserData *> (user_data);

    strcpy ((char *)(data->request_str), (const char *)requestStr);
    data->cloud_candidates = this;

    /* record the latest timer */
    event_id = m_source_event_id = g_timeout_add_full (G_PRIORITY_DEFAULT, m_delayed_time, delayedCloudAsyncRequestCallBack, user_data, delayedCloudAsyncRequestDestroyCallBack);
    data->event_id = event_id;
}

void
CloudCandidates::cloudAsyncRequest (const gchar* requestStr, std::vector<EnhancedCandidate> & candidates)
{
    cloudAsyncRequest (requestStr);
}

void
CloudCandidates::cloudAsyncRequest (const gchar* requestStr)
{
    GError **error = NULL;
    gchar *queryRequest;
    if (m_cloud_source == BAIDU)
        queryRequest= g_strdup_printf (BAIDU_URL_TEMPLATE, requestStr, m_cloud_candidates_number);
    else if (m_cloud_source == GOOGLE)
        queryRequest= g_strdup_printf (GOOGLE_URL_TEMPLATE, requestStr, m_cloud_candidates_number);

    /* cancel message if there is a pending one */
    if (m_message)
        soup_session_cancel_message (m_session, m_message, SOUP_STATUS_CANCELLED);

    SoupMessage *msg = soup_message_new ("GET", queryRequest);
    soup_session_send_async (m_session, msg, NULL, cloudResponseCallBack, static_cast<gpointer> (this));
    m_message = msg;

    /* cache the last request string */
    strcpy (m_last_requested_pinyin, requestStr);

    /* update loading text to replace pending text */
    for (std::vector<EnhancedCandidate>::iterator pos = m_candidates.begin (); pos != m_candidates.end (); ++pos) {
        pos->m_display_string = CANDIDATE_LOADING_TEXT_WITHOUT_PREFIX;
    }

    /* only update lookup table when there is still pinyin text */
    if (strlen (m_editor->m_text) >= CLOUD_MINIMUM_TRIGGER_LENGTH)
        updateLookupTable ();
}

void
CloudCandidates::cloudResponseCallBack (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    GError **error = NULL;
    GInputStream *stream = soup_session_send_finish (SOUP_SESSION (source_object), result, error);
    CloudCandidates *cloudCandidates = static_cast<CloudCandidates *> (user_data);

    /* process results */
    cloudCandidates->processCloudResponse (stream, cloudCandidates->m_editor->m_candidates);

    /* only update lookup table when there is still pinyin text */
    if (strlen (cloudCandidates->m_editor->m_text) >= CLOUD_MINIMUM_TRIGGER_LENGTH) {
        cloudCandidates->updateLookupTable ();

        /* clean up message */
        cloudCandidates->m_message = NULL;
    }
}

void
CloudCandidates::cloudSyncRequest (const gchar* requestStr, std::vector<EnhancedCandidate> & candidates)
{
    GError **error = NULL;
    gchar *queryRequest;
    if (m_cloud_source == BAIDU)
        queryRequest= g_strdup_printf (BAIDU_URL_TEMPLATE, requestStr, m_cloud_candidates_number);
    else if (m_cloud_source == GOOGLE)
        queryRequest= g_strdup_printf (GOOGLE_URL_TEMPLATE, requestStr, m_cloud_candidates_number);
    SoupMessage *msg = soup_message_new ("GET", queryRequest);

    GInputStream *stream = soup_session_send (m_session, msg, NULL, error);

    processCloudResponse (stream, candidates);
}

void
CloudCandidates::processCloudResponse (GInputStream *stream, std::vector<EnhancedCandidate> & candidates)
{
    guint ret_code;
    CloudCandidatesResponseJsonParser *parser = NULL;
    const gchar *text = NULL;
    gchar *double_pinyin_text = NULL;
    gchar annotation[MAX_PINYIN_LEN + 1];

    if (m_cloud_source == BAIDU)
        parser = new BaiduCloudCandidatesResponseJsonParser ();
    else if (m_cloud_source == GOOGLE)
        parser = new GoogleCloudCandidatesResponseJsonParser ();

    ret_code = parser->parse (stream);

    /* no annotation if there is NETWORK_ERROR, process before detecting parsed annotation,  */
    if (ret_code == PARSER_NETWORK_ERROR) {
        for (std::vector<EnhancedCandidate>::iterator pos = m_candidates.begin (); pos != m_candidates.end (); ++pos) {
            pos->m_display_string = CANDIDATE_INVALID_DATA_TEXT_WITHOUT_PREFIX;
        }
    }

    if (parser->getAnnotation ())
        strcpy (annotation, parser->getAnnotation ());
    else {
        /* the request might have been cancelled */
        return;
    }

    if (! m_editor->m_config.doublePinyin ()) {
        /* get current text in editor */
        text = m_editor->m_text;
    }
    else {
        /* get current double pinyin text */
        String stripped = m_editor->m_buffer;
        const gchar *temp= stripped;
        gchar** tempArray =  g_strsplit_set (temp, " |", -1);
        double_pinyin_text = g_strjoinv ("", tempArray);
        g_strfreev (tempArray);
    }

    if (m_cloud_source == BAIDU || !g_strcmp0 (annotation, text) || !g_strcmp0 (annotation, double_pinyin_text)) {
        if (ret_code == PARSER_NOERR) {
            /* update to the cached candidates list */
            std::vector<std::string> &updated_candidates = parser->getStringCandidates ();

            std::vector<EnhancedCandidate>::iterator cached_candidate_pos = m_candidates.begin ();
            for (guint i = 0; cached_candidate_pos != m_candidates.end () && i < updated_candidates.size (); ++i, ++cached_candidate_pos) {
                /* cache candidate without prefix in m_candidates */
                EnhancedCandidate & cached = *cached_candidate_pos;
                cached.m_display_string = updated_candidates[i];
            }
        }
        else {
            String text;

            switch (ret_code) {
            case PARSER_NO_CANDIDATE:
                text = CANDIDATE_NO_CANDIDATE_TEXT_WITHOUT_PREFIX;
                break;
            case PARSER_INVALID_DATA:
                text = CANDIDATE_INVALID_DATA_TEXT_WITHOUT_PREFIX;
                break;
            case PARSER_BAD_FORMAT:
                text = CANDIDATE_BAD_FORMAT_TEXT_WITHOUT_PREFIX;
                break;
            }

            for (std::vector<EnhancedCandidate>::iterator pos = m_candidates.begin (); pos != m_candidates.end (); ++pos) {
                pos->m_display_string = text;
            }
        }
    }

    if (parser)
        delete parser;

    if (double_pinyin_text)
        g_free (double_pinyin_text);
}

void
CloudCandidates::updateLookupTable ()
{
    /* retrieve cursor position in lookup table */
    guint cursor = m_editor->m_lookup_table.cursorPos ();

    /* update cached cloud input candidates */
    m_editor->updateCandidates ();

    /* regenerate lookup table */
    m_editor->m_lookup_table.clear ();
    m_editor->fillLookupTable ();

    /* recover cursor position in lookup table */
    m_editor->m_lookup_table.setCursorPos (cursor);

    /* notify ibus */
    m_editor->updateLookupTableFast ();
}

std::vector<EnhancedCandidate> CloudCandidatesResponseParser::getCandidates ()
{
    std::vector<EnhancedCandidate> candidates;

    for (std::vector<std::string>::const_iterator i = m_candidates.cbegin (); i != m_candidates.cend (); ++i) {
        EnhancedCandidate candidate;
        candidate.m_candidate_type = CANDIDATE_CLOUD_INPUT;
        candidate.m_display_string = *i;
        candidates.push_back (candidate);
    }

    return candidates;
}

CloudCandidatesResponseJsonParser::CloudCandidatesResponseJsonParser () : m_parser (NULL)
{
    m_parser = json_parser_new ();
}

CloudCandidatesResponseJsonParser::~CloudCandidatesResponseJsonParser ()
{
    /* free json parser object if necessary */
    if (m_parser)
        g_object_unref (m_parser);
}
