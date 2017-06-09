/* Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#include "session_tracker.h"

#include "hash.h"
#include "rpl_gtid.h"
#include "sql_class.h"
#include "sql_show.h"

/* To be used in expanding the buffer. */
static const unsigned int EXTRA_ALLOC= 1024;

///////////////////////////////////////////////////////////////////////////////

/**
  This is an interface for encoding the gtids in the payload of the
  the OK packet.

  In the future we may have different types of payloads, thence we may have
  different encoders specifications/types. This implies changing either, the
  encoding specification code, the actual encoding procedure or both at the
  same time.

  New encoders can extend this interface/abstract class or extend
  other encoders in the hierarchy.
*/
class Session_gtids_ctx_encoder
{
public:
  Session_gtids_ctx_encoder() {}
  virtual ~Session_gtids_ctx_encoder() {};

  /*
   This function SHALL encode the collected GTIDs into the buffer.
   @param thd The session context.
   @param buf The buffer that SHALL contain the encoded data.
   @return false if the contents were successfully encoded, true otherwise.
           if the return value is true, then the contents of the buffer is
           undefined.
   */
  virtual bool encode(THD *thd, String& buf)= 0;

  /*
   This function SHALL return the encoding specification used in the
   packet sent to the client. The format of the encoded data will differ
   according to the specification set here.

   @return the encoding specification code.
   */
  virtual ulonglong encoding_specification()= 0;
private:
  // not implemented
  Session_gtids_ctx_encoder(const Session_gtids_ctx_encoder& rsc);
  Session_gtids_ctx_encoder& operator=(const Session_gtids_ctx_encoder& rsc);
};

class Session_gtids_ctx_encoder_string : public Session_gtids_ctx_encoder
{
public:

  Session_gtids_ctx_encoder_string() {}
  ~Session_gtids_ctx_encoder_string() {}

  ulonglong encoding_specification() { return 0; }

  bool encode(THD *thd, String& buf)
  {
    const char *gtid= thd->get_trans_gtid();

    if (gtid)
    {
      /*
        No need to use net_length_size in the following two fields.
        These are constants in this class and will both be encoded using
        only 1 byte.
      */
      /* net_length_size((ulonglong)SESSION_TRACK_GTIDS); */
      ulonglong tracker_type_enclen= 1;
      /* net_length_size(encoding_specification()); */;
      ulonglong encoding_spec_enclen= 1;
      ulonglong gtids_string_len= strlen(gtid);
      ulonglong gtids_string_len_enclen= net_length_size(gtids_string_len);
      ulonglong entity_len= encoding_spec_enclen + gtids_string_len_enclen +
                            gtids_string_len;
      ulonglong entity_len_enclen= net_length_size(entity_len);
      ulonglong total_enclen= tracker_type_enclen + entity_len_enclen +
                              encoding_spec_enclen + gtids_string_len_enclen +
                              gtids_string_len;

      /* prepare the buffer */
      uchar *to= (uchar *) buf.prep_append(total_enclen, EXTRA_ALLOC);

     /* format of the payload is as follows:
       [ tracker type] [len] [ encoding spec ] [gtid string len] [gtid string]
      */

      /* Session state type (SESSION_TRACK_SCHEMA) */
      *to= (uchar) SESSION_TRACK_GTIDS; to++;

      /* Length of the overall entity. */
      to= net_store_length(to, entity_len);

      /* encoding specification */
      *to= (uchar) encoding_specification(); to++;

      /* the actual gtid set string */
      to= net_store_data(to, (uchar*) gtid, gtids_string_len);
    }
    return false;
  }
private:
  // not implemented
  Session_gtids_ctx_encoder_string(const Session_gtids_ctx_encoder_string& rsc);
  Session_gtids_ctx_encoder_string& operator=(
      const Session_gtids_ctx_encoder_string& rsc);
};

/**
  Session_gtids_tracker
  ---------------------------------
  This is a tracker class that enables & manages the tracking of gtids for
  relaying to the connectors the information needed to handle session
  consistency.
*/

class Session_gtids_tracker : public State_tracker
{
private:
  void reset();
  Session_gtids_ctx_encoder* m_encoder;

public:

  /** Constructor */
  Session_gtids_tracker() : m_encoder(NULL)
  { }

  ~Session_gtids_tracker()
  {
    if (m_encoder)
      delete m_encoder;
  }

  bool is_changed(THD* thd) const override
  { return thd->get_trans_gtid() != NULL; }
  bool enable(THD *thd) override
  { return update(thd); }
  bool check(THD *thd, set_var *var) override
  { return false; }
  bool update(THD *thd) override;
  bool store(THD *thd, String &buf) override;
  void mark_as_changed(THD *thd, LEX_CSTRING *tracked_item_name) override;
};


///////////////////////////////////////////////////////////////////////////////

/** Constructor */
Session_state_change_tracker::Session_state_change_tracker()
{
  m_changed= false;
}

/**
  @brief Initiate the value of m_enabled based on
  @@session_track_state_change value.

  @param thd [IN]           The thd handle.
  @return                   false (always)

**/

bool Session_state_change_tracker::enable(THD *thd)
{
  m_enabled= (thd->variables.session_track_state_change)? true: false;
  return false;
}

/**
  @Enable/disable the tracker based on @@session_track_state_change value.

  @param thd [IN]           The thd handle.
  @return                   false (always)

**/

bool Session_state_change_tracker::update(THD *thd)
{
  return enable(thd);
}

/**
  @brief Store the 1byte boolean flag in the specified buffer. Once the
         data is stored, we reset the flags related to state-change. If
         1byte flag valie is 1 then there is a session state change else
         there is no state change information.

  @param thd [IN]           The thd handle.
  @paran buf [INOUT]        Buffer to store the information to.

  @return
    false                   Success
    true                    Error
**/

bool Session_state_change_tracker::store(THD *thd, String &buf)
{
  /* since its a boolean tracker length is always 1 */
  const ulonglong length= 1;

  uchar *to= (uchar *) buf.prep_append(3,EXTRA_ALLOC);

  /* format of the payload is as follows:
     [ tracker type] [length] [1 byte flag] */

  /* Session state type (SESSION_TRACK_STATE_CHANGE) */
  to= net_store_length(to, (ulonglong)SESSION_TRACK_STATE_CHANGE);

  /* Length of the overall entity it is always 1 byte */
  to= net_store_length(to, length);

  /* boolean tracker will go here */
  *to= (is_state_changed(thd) ? '1' : '0');

  reset();

  return false;
}

/**
  @brief Mark the tracker as changed and associated session
         attributes accordingly.

  @param name [IN]          Always null.
  @return void
*/

void Session_state_change_tracker::mark_as_changed(THD *thd,
                                                   LEX_CSTRING *tracked_item_name)
{
  /* do not send the boolean flag for the tracker itself
     in the OK packet */
  if(tracked_item_name &&
     (strncmp(tracked_item_name->str, "session_track_state_change", 26) == 0))
    m_changed= false;
  else
  {
    m_changed= true;
    thd->lex->safe_to_cache_query= 0;
  }
}

/**
  @brief Reset the m_changed flag for next statement.

  @return                   void
*/

void Session_state_change_tracker::reset()
{
  m_changed= false;
}

/**
  @brief find if there is a session state change

  @return
  true  - if there is a session state change
  false - if there is no session state change
**/

bool Session_state_change_tracker::is_state_changed(THD* thd)
{
  return m_changed;
}

///////////////////////////////////////////////////////////////////////////////

/**
  @brief Initialize session tracker objects.

  @param char_set [IN]      The character set info.

  @return                   void
*/

void Session_tracker::init(const CHARSET_INFO *char_set)
{
  m_trackers[SESSION_GTIDS_TRACKER]=
    new (std::nothrow) Session_gtids_tracker;
  m_trackers[SESSION_STATE_CHANGE_TRACKER]=
    new (std::nothrow) Session_state_change_tracker;
}

void Session_tracker::claim_memory_ownership()
{
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
    m_trackers[i]->claim_memory_ownership();
}

/**
  @brief Enables the tracker objects.

  @param thd [IN]    The thread handle.

  @return            void
*/
void Session_tracker::enable(THD *thd)
{
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
    m_trackers[i]->enable(thd);
}

/**
  @brief Returns the pointer to the tracker object for the specified tracker.

  @param tracker [IN]       Tracker type.

  @return                   Pointer to the tracker object.
*/

State_tracker *
Session_tracker::get_tracker(enum_session_tracker tracker) const
{
  return m_trackers[tracker];
}


/**
  @brief Checks if m_enabled flag is set for any of the tracker objects.

  @return
    true  - At least one of the trackers is enabled.
    false - None of the trackers is enabled.

*/

bool Session_tracker::enabled_any()
{
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
  {
    if (m_trackers[i]->is_enabled())
      return true;
  }
  return false;
}

/**
  @brief Checks if m_changed flag is set for any of the tracker objects.

  @return
    true                    At least one of the entities being tracker has
                            changed.
    false                   None of the entities being tracked has changed.
*/

bool Session_tracker::changed_any(THD* thd)
{
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
  {
    if (m_trackers[i]->is_changed(thd))
      return true;
  }
  return false;
}


/**
  @brief Store all change information in the specified buffer.

  @param thd [IN]           The thd handle.
  @param buf [OUT]          Reference to the string buffer to which the state
                            change data needs to be written.

  @return                   void
*/

void Session_tracker::store(THD *thd, String &buf)
{
  /* Temporary buffer to store all the changes. */
  String temp;
  size_t length;

  /* Get total length. */
  for (int i= 0; i <= SESSION_TRACKER_END; i ++)
  {
    if (m_trackers[i]->is_changed(thd))
      m_trackers[i]->store(thd, temp);
  }

  length= temp.length();
  /* Store length first.. */
  char *to= buf.prep_append(net_length_size(length), EXTRA_ALLOC);
  net_store_length((uchar *) to, length);

  /* .. and then the actual info. */
  buf.append(temp);
  temp.free();
}


/**
  @brief Enable/disable the tracker based on @@session_track_gtids's value.

  @param thd [IN]           The thd handle.

  @return
    false (always)
*/

bool Session_gtids_tracker::update(THD *thd)
{
  /*
    We are updating this using the previous value. No change needed.
    Bailing out.
  */
  if (m_enabled == (thd->variables.session_track_gtids != OFF))
    return false;

  m_enabled= thd->variables.session_track_gtids != OFF &&
             /* No need to track GTIDs for system threads. */
             thd->system_thread == NON_SYSTEM_THREAD;
  if (m_enabled)
  {
    // instantiate the encoder if needed
    if (m_encoder == NULL)
    {
      /*
       TODO: in the future, there can be a variable to control which
       encoder instance to instantiate here.

       This means that if we ever make the server encode deltas instead,
       or compressed GTIDS we want to change the encoder instance below.

       Right now, by default we instantiate the encoder that has.
      */
      m_encoder= new Session_gtids_ctx_encoder_string();
    }
  }
  // else /* break the bridge between tracker and collector */
  return false;
}

/**
  @brief Store the collected gtids as length-encoded string in the specified
         buffer.  Once the data is stored, we reset the flags related to
         state-change (see reset()).


  @param thd [IN]           The thd handle.
  @paran buf [INOUT]        Buffer to store the information to.

  @return
    false                   Success
    true                    Error
*/

bool Session_gtids_tracker::store(THD *thd, String &buf)
{
  if (m_encoder && m_encoder->encode(thd, buf))
    return true;
  reset();
  return false;
}

/**
  @brief Mark the tracker as changed.

  @param thd               [IN]          Always null.
  @param tracked_item_name [IN]          Always null.

  @return void
*/

void Session_gtids_tracker::mark_as_changed(THD *thd MY_ATTRIBUTE((unused)),
                                            LEX_CSTRING *tracked_item_name
                                            MY_ATTRIBUTE((unused)))
{
  m_changed= true;
}


/**
  @brief Reset the m_changed flag for next statement.

  @return                   void
*/

void Session_gtids_tracker::reset()
{
  /*
   Delete the encoder and remove the listener if this had been previously
   deactivated.
   */
  if (!m_enabled && m_encoder)
  {
    // delete the encoder (just to free memory)
    delete m_encoder; // if not tracking, delete the encoder
    m_encoder= NULL;
  }
  m_changed= false;
}
