#include "rpl_slave_snapshot_manager.h"
#include "debug_sync.h"

bool Snapshot_manager::init()
{
  DBUG_ASSERT(m_rli->mts_groups_assigned == 0);
  return update_snapshot(true);
}

bool Snapshot_manager::update_snapshot(bool force)
{
  const auto lwm_seqno= m_rli->gaq->lwm.total_seqno;
  bool ret= true;

  mysql_mutex_lock(&m_mutex);

  DBUG_ASSERT(force || lwm_seqno <= m_next_seqno);

  if (!force && lwm_seqno < m_next_seqno)
  {
    goto end;
  }

  DBUG_ASSERT(force || lwm_seqno == m_next_seqno);

  if (m_rli->info_thd->create_explicit_snapshot(false))
  {
    sql_print_error("Error while creating explicit snapshot!");
    ret= false;
    goto end;
  }

  m_snapshot= m_rli->info_thd->get_explicit_snapshot();

  DBUG_ASSERT(force || m_rli->mts_groups_assigned >= m_next_seqno);
  m_next_seqno= m_rli->mts_groups_assigned;

  mysql_cond_broadcast(&m_cond);

end:
  mysql_mutex_unlock(&m_mutex);

  DBUG_EXECUTE_IF(
      "wait_for_repl_update_snapshot",
      {
        const char act[]= "now signal reached wait_for continue";
        DBUG_ASSERT(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
      };
  );

  return ret;
}
