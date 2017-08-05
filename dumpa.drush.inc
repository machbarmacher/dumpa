<?php
/**
 * @file dumpa.drush.inc
 */

use Drush\Log\LogLevel;

/**
 * Implements hook_drush_command().
 */
function dumpa_drush_command() {
  $items = array();

  $items['dumpa-dump'] = array(
    'aliases' => ['dumpa'],
    'callback' => 'dumpa_dump',
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_DATABASE,
    'description' => 'Dump DB to single files.',
    'options' => _dumpa_table_selection_options() + [
      'result-dir' => 'Result directory. Defaults to ../private/_db_$site.',
      'site' => 'Site to use in default result directory. Defaults to "default".',
    ],
  );

  $items['dumpa-restore'] = array(
    'aliases' => ['restora'],
    'callback' => 'dumpa_restore',
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_DATABASE,
    'description' => 'Restore DB from single files.',
    'options' => _dumpa_table_selection_options() + [
      'result-dir' => 'Result directory. Defaults to ../private/_db_$site.',
      'site' => 'Site to use in default result directory. Defaults to "default".',
    ],
  );
  return $items;
}

function _dumpa_table_selection_options() {
  // Copy of drush_sql_get_table_selection_options().
  return array(
    'skip-tables-key' => 'A key in the $skip_tables array. @see example.drushrc.php. Optional.',
    'structure-tables-key' => 'A key in the $structure_tables array. @see example.drushrc.php. Optional.',
    'tables-key' => 'A key in the $tables array. Optional.',
    'skip-tables-list' => 'A comma-separated list of tables to exclude completely. Optional.',
    'structure-tables-list' => 'A comma-separated list of tables to include for structure, but not data. Optional.',
    'tables-list' => 'A comma-separated list of tables to transfer. Optional.',
  );
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
function dumpa_dump() {
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
    if (!_dumpa_dump_structure($sql, $tmp, $table)) {
      return FALSE;
    }
  }
  foreach ($data_tables as $table) {
    if (!_dumpa_dump_data($sql, $tmp, $table)) {
      return FALSE;
    }
  }
  if (!drush_get_context('DRUSH_SIMULATE')) {
    drush_delete_dir($dir);
    drush_move_dir($tmp, $dir);
  }
  return TRUE;
}

function _dumpa_dump_structure(Drush\Sql\SqlBase $sql, $dir, $table) {
  $cmd = _dumpa_cmd($sql, $table, FALSE);
  $file_escaped = drush_escapeshellarg("$dir/$table.sql");
  if (!$return = drush_op_system("($cmd) > $file_escaped")) {
    drush_log(dt('Table structure dumped: !table', array('!table' => $table)), LogLevel::SUCCESS);
  }
  else {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Table structure dump failed: !table', array('!table' => $table)));
  }
  return TRUE;
}

function _dumpa_dump_data($sql, $dir, $table) {
  $cmd = _dumpa_cmd($sql, $table, FALSE);
  $file_escaped = drush_escapeshellarg("$dir/$table.txt");
  if (!$return = drush_op_system("($cmd) > $file_escaped")) {
    drush_log(dt('Table data dumped: !table', array('!table' => $table)), LogLevel::SUCCESS);
  }
  else {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Table data dump failed: !table', array('!table' => $table)));
  }
  return TRUE;
}

function _dumpa_cmd(Drush\Sql\SqlBase $sql, $table, $csv) {
  // @see \Drush\Sql\Sqlmysql::dumpCmd
  $cmd = 'mysqldump ';
  // DB name.
  $cmd .= str_replace('--database=', ' ', $sql->creds());
  // Structure only.
  $cmd .= " --no-data";
  $cmd -= ' --no-autocommit --single-transaction --opt -Q';
  // Ordered dump.
  $cmd .= ' --skip-extended-insert --order-by-primary';
  if ($csv) {
    $cmd .= ' --fields-terminated-by="," --fields-enclosed-by="\"" --lines-terminated-by="\r\n"';
  }
  $cmd .= " $table";
  return $cmd;
}

function dumpa_restore() {
  $sql = drush_sql_get_class();
  // @todo Assert mysql.

  $site = drush_get_option('site', 'default');
  $dir = drush_get_option('result-dir', "../private/_db_$site");

  $sql_files = _dumpa_files($dir, '.sql');
  $txt_files = _dumpa_files($dir, '.txt');

  // Calculate $tables and $data_tables.
  $table_selection = $sql->get_expanded_table_selection();
  $tables = $table_selection['tables']
    ?: $sql->listTables();
  $skip_tables = $table_selection['skip'];
  $tables = array_diff($tables, $skip_tables);
  $data_tables = array_diff($tables, $table_selection['structure']);

  // Import all table structure in one command.
  // We use a named pipe to feed all sql into one query.
  // SQL class does not allow to have multiple input files.
  $fifo = _dumpa_fifo();
  $cmd = "cat ";
  foreach (array_intersect_key($sql_files, array_flip($tables)) as $file) {
    $cmd .= " " . drush_escapeshellarg($file);
  }
  $cmd .= ">" . drush_escapeshellarg($fifo);
  if ($success = drush_op_system($cmd)) {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Could not cat data to fifo.'));
  }
  if ($success = $sql->query(NULL, $fifo)) {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Could not import.'));
  }
  unlink($fifo);

  // Import all table data in one command.
  // @link https://dev.mysql.com/doc/refman/5.7/en/reloading-delimited-text-dumps.html
  $cmd = "mysqlimport ";
  // DB name.
  $cmd .= str_replace('--database=', ' ', $sql->creds());
  $cmd .= ' --fields-terminated-by="," --fields-enclosed-by="\"" --lines-terminated-by="\r\n"';
  foreach (array_intersect_key($txt_files, array_flip($data_tables)) as $file) {
    $cmd .= " " . drush_escapeshellarg($file);
  }
  if (!$success = drush_op_system($cmd)) {
    drush_log(dt('All table data restored'), LogLevel::SUCCESS);
  }
  else {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('All table data restore failed.'));
  }
  return TRUE;
}

function _dumpa_fifo() {
  // Adapted from drush_tempdir()
  $tmp = drush_trim_path(drush_find_tmp());
  $tmp .= '/' . 'drush_fifo_' . uniqid(time() . '_');

  posix_mkfifo($tmp, 0600);
  drush_register_file_for_deletion($tmp);

  return $tmp;
}

function _dumpa_files($dir, $suffix) {
  $files = glob("$dir/*.$suffix");
  $indexed_files = [];
  foreach ($files as $file) {
    $basename = basename($file);
    $index = substr($basename, -strlen($suffix));
    $indexed_files[$index] = $file;
  }
  return $indexed_files;
}
