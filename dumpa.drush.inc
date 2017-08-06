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
    'options' => _dumpa_common_options(),
  );

  $items['dumpa-restore'] = array(
    'aliases' => ['restora'],
    'callback' => 'dumpa_restore',
    'bootstrap' => DRUSH_BOOTSTRAP_DRUPAL_DATABASE,
    'description' => 'Restore DB from single files.',
    'options' => _dumpa_common_options(),
  );
  return $items;
}

function _dumpa_common_options() {
  // Copy of drush_sql_get_table_selection_options().
  return array(
    'drop' => 'Use --no-drop to suppress dropping of additional files / tables.',
    'result-dir' => 'Result directory. Defaults to ../private/_db_$site.',
    'site' => 'Site to use in default result directory. Defaults to "default".',
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
 * So we simulate this in userland, by dumping tables to separate
 * files, and using sql for data too.
 *
 * @link https://stackoverflow.com/a/6300583/606859
 * @link https://stackoverflow.com/a/12041241/606859
 */
function dumpa_dump() {
  // @see drush_sql_dump(), @see \Drush\Sql\SqlBase::dump()
  $sql = drush_sql_get_class();
  if (!$sql instanceof \Drush\Sql\Sqlmysql) {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Can only work with mysql currently.'));
  }
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
    $dump_data_too = in_array($table, $data_tables);
    if (!_dumpa_dump_table($sql, $tmp, $table, $dump_data_too)) {
      return FALSE;
    }
  }

  // Move files and keep track of them.
  $source_files = glob("$tmp/*.sql");
  $moved_files = [];
  foreach ($source_files as $source_file) {
    $base_filename = basename($source_file);
    $destination_file = "$dir/" . $base_filename;
    // Looks like drush_move_dir() also works for files.
    $success = drush_move_dir($source_file, $destination_file, TRUE);
    if (!$success) {
      return drush_set_error('DRUSH_DUMPA_FAIL', dt('Can not copy file: !file', ['!file' => $base_filename]));
    }
    $moved_files[] = $destination_file;
  }
  // Drop other files.
  if (drush_get_option('drop', TRUE)) {
    $destination_files = glob("$dir/*.sql");
    foreach (array_diff($destination_files, $moved_files) as $destination_file) {
      // Use drush_op to respect --simulate.
      $success = drush_op('unlink', $destination_file);
      if (!$success) {
        return drush_set_error('DRUSH_DUMPA_FAIL', dt('Can not delete file: !file', ['!file' => $destination_file]));
      }
    }
  }
  return TRUE;
}

function _dumpa_dump_table(Drush\Sql\SqlBase $sql, $dir, $table, $dump_data_too) {
  return _dumpa_dump_component($sql, $dir, $table, FALSE)
    && ($dump_data_too ? _dumpa_dump_component($sql, $dir, $table, TRUE) : TRUE);
}

function _dumpa_dump_component(Drush\Sql\SqlBase $sql, $dir, $table, $dump_data) {
  $cmd = _dumpa_cmd($sql, $table, $dump_data);
  $component = $dump_data ? 'data' : 'structure';
  $cmd .= ' >' . drush_escapeshellarg("$dir/$table.{$component}.sql");
  $t_args = array('!table' => $table, '!component' => $component);
  // No need to capture output.
  // Mind that drush_op_system return code is 0 on success.
  $success = !drush_op_system($cmd);
  if (!$success) {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Table !component dump failed: !table', $t_args));
  }
  else {
    drush_log(dt('Table !component dumped: !table', $t_args), LogLevel::SUCCESS);
  }
  return TRUE;
}

function _dumpa_cmd(Drush\Sql\SqlBase $sql, $table, $dump_data) {
  // @see \Drush\Sql\Sqlmysql::dumpCmd
  $cmd = 'mysqldump ';
  // DB name.
  $cmd .= str_replace('--database=', ' ', $sql->creds());
  if ($dump_data) {
    $cmd .= ' --no-create-info';
  }
  else {
    $cmd .= " --no-data";
  }
  $cmd .= ' --no-autocommit --single-transaction --opt -Q';
  // Ordered dump.
  $cmd .= ' --skip-extended-insert --order-by-primary';
  $cmd .= " $table";
  return $cmd;
}

function dumpa_restore() {
  $sql = drush_sql_get_class();
  if (!$sql instanceof \Drush\Sql\Sqlmysql) {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Can only work with mysql currently.'));
  }

  $site = drush_get_option('site', 'default');
  $dir = drush_get_option('result-dir', "../private/_db_$site");

  $structure_files = _dumpa_files($dir, '.structure.sql');
  $data_files = _dumpa_files($dir, '.data.sql');

  if (!$structure_files && !$data_files) {
    return drush_set_error('DRUSH_DUMPA_FAIL', dt('Dumpa directory is empty.'));
  }

  // Calculate $tables and $data_tables.
  $table_selection = $sql->get_expanded_table_selection();
  $tables = $table_selection['tables']
    ?: $sql->listTables();
  $skip_tables = $table_selection['skip'];
  $tables = array_diff($tables, $skip_tables);
  $data_tables = array_diff($tables, $table_selection['structure']);

  // Lock.
  $sql->query(sprintf('LOCK TABLES %s WRITE', implode(', ', $tables)));

  $restored_tables = [];
  foreach (array_intersect_key($structure_files, array_flip($tables)) as $table => $file) {
    $success = $sql->query(NULL, $file);
    if (!$success) {
      return drush_set_error('DRUSH_DUMPA_FAIL', dt('Could not import structure: !table.', ['!table' => $table]));
    }
    $restored_tables[] = $table;
  }
  foreach (array_intersect_key($data_files, array_flip($tables), array_flip($data_tables)) as $table => $file) {
    if (!in_array($table, $restored_tables)) {
      return drush_set_error('DRUSH_DUMPA_FAIL', dt('Found data without structure: !table.', ['!table' => $table]));
    }
    $success = $sql->query(NULL, $file);
    if (!$success) {
      return drush_set_error('DRUSH_DUMPA_FAIL', dt('Could not import data: !table.', ['!table' => $table]));
    }
  }

  // Unlock
  $sql->query('UNLOCK TABLES');

  // Drop tables not restored (and not excluded).
  $tables_to_drop = array_diff($tables, $restored_tables);
  if ($tables_to_drop && drush_get_option('drop', TRUE)) {
    drush_log(dt('Dropping tables: !tables', ['!tables' => implode(', ', $tables_to_drop)]));
    $sql->drop($tables_to_drop);
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
  $files = glob("$dir/*$suffix");
  $indexed_files = [];
  foreach ($files as $file) {
    $basename = basename($file);
    $index = substr($basename, 0, -strlen($suffix));
    $indexed_files[$index] = $file;
  }
  return $indexed_files;
}
