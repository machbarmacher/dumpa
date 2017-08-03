<?php
/**
 * @file drush_sql_dumpa.drush.inc
 */

/**
 * Implements hook_drush_command().
 */
function drush_sql_dumpa_drush_command() {
  $items = array();

  $items['sql-dumpa'] = array(
    'callback' => 'drush_sql_dumpa_dump',
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_DATABASE,
    'description' => 'Dump DB to single files.',
    'options' => [
      'result-dir' => 'Result directory. Defaults to ../private/_db_$site.',
      'site' => 'Site to use in default result directory. Defaults to "default".',
    ],
  );
  return $items;
}

/**
 * Dump DB to single files.
 *
 * The mysqldump option --tab dumps tables to single files and
 * even splits them into sql schema and csv data. Great, but to
 * work the DB needs the permission to access and modify any files
 * on the server so a sane admin won't give this to mortal users
 * in a shared hosting environment. Sigh.
 * So we simulate this in userland, by dumping tables to separate files.
 *
 * @link https://stackoverflow.com/a/6300583/606859
 * @link https://stackoverflow.com/a/12041241/606859
 */
function drush_sql_dumpa_dump() {
  // @see drush_sql_dump(), @see \Drush\Sql\SqlBase::dump()
  $sql = drush_sql_get_class();
  // @todo Assert mysql.
  $tmp = drush_tempdir();

  $site = drush_get_option('site', 'default');
  $dir = drush_get_option('result-dir', "../private/_db_$site");

  // Calculate $tables and $data_tables.
  $table_selection = $sql->get_expanded_table_selection();
  $tables = $table_selection['tables']
    ?: $sql->listTables();
  $tables = array_diff($tables, $table_selection['skip']);
  $data_tables = array_diff($tables, $table_selection['structure']);

  foreach ($tables as $table) {
    if (!_drush_sql_dumpa_dump_structure($sql, $tmp, $table)) {
      return FALSE;
    }
  }
  foreach ($data_tables as $table) {
    if (!_drush_sql_dumpa_dump_data($sql, $tmp, $table)) {
      return FALSE;
    }
  }
  if (!drush_get_context('DRUSH_SIMULATE')) {
    drush_delete_dir($dir);
    drush_move_dir($tmp, $dir);
  }
  return TRUE;
}

function _drush_sql_dumpa_dump_structure(Drush\Sql\SqlBase $sql, $dir, $table) {
  $cmd = _drush_sql_dumpa_cmd($sql, $table, FALSE);
  if (!$return = drush_op_system($cmd)) {
    drush_log(dt('Table structure dumped: !table', array('!table' => $table)), LogLevel::SUCCESS);
  }
  else {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Table structure dump failed: !table', array('!table' => $table)));
  }
  return TRUE;
}

function _drush_sql_dumpa_dump_data($sql, $dir, $table) {
  $cmd = _drush_sql_dumpa_cmd($sql, $table, FALSE);
  if (!$return = drush_op_system($cmd)) {
    drush_log(dt('Table data dumped: !table', array('!table' => $table)), LogLevel::SUCCESS);
  }
  else {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Table data dump failed: !table', array('!table' => $table)));
  }
  return TRUE;
}

function _drush_sql_dumpa_cmd(Drush\Sql\SqlBase $sql, $table, $csv) {
// @see \Drush\Sql\Sqlmysql::dumpCmd
  $cmd = 'mysqldump ';
  // DB name.
  $cmd .= str_replace('--database=', ' ', $sql->creds());
  // Structure only.
  $cmd .= " --no-data";
  $cmd = ' --no-autocommit --single-transaction --opt -Q';
  // Ordered dump.
  $cmd .= ' --skip-extended-insert --order-by-primary';
  if ($csv) {
    $cmd .= ' --fields-terminated-by="," --fields-enclosed-by="\"" --lines-terminated-by="\r\n"';
  }
  $cmd .= " $table";
  return $cmd;
}
