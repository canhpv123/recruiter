<?php

/**
 * @file
 *
 */

/**
 * Helper function for generating a regex from a list of paths.
 *
 * Generates a single regex from a list of file paths that can be used to match
 * JS or CSS files using preg_grep() for example in hook_css_alter() or
 * hook_js_alter(). The '*' (asterisk) character can be used as a wild-card.
 *
 * @param array $paths
 *   An array of file paths.
 *
 * @return string
 *   The generated regex.
 *
 * @see hook_js_alter()
 * @see hook_css_alter()
 */
function omega_assets_prepare_regex(array $paths) {
  // Don't do any processing if no paths are given.
  if (empty($paths)) {
    return FALSE;
  }

  $profile = drupal_get_profile();
  $site = preg_quote(conf_path(), '/');

  $themes = array();
  foreach ($GLOBALS['base_theme_info'] as $info) {
    $themes[] = preg_quote(dirname($info->filename), '/');
  }

  $tokens = array(
    ':core' => '(?:misc|modules|themes)',
    ':contrib' => "(?:sites\\/all\\/modules|$site\\/modules|profiles\\/$profile\\/modules)",
    ':current-theme' => preg_quote(drupal_get_path('theme', $GLOBALS['theme_key']), '/'),
    ':base-theme' => $themes ? '(?:' . implode('|', $themes) . ')' : FALSE,
  );

  if (!$cache = cache_get("omega:omega_assets_prepare_regex:tokens")) {
    $system_tokens = array();
    // Add all active modules to the token list.
    foreach (module_list() as $module) {
      $directory = preg_quote(drupal_get_path('module', $module), '/');
      $system_tokens[$module] = $directory;
    }
    // Add all themes and engines to the token list.
    foreach (list_themes() as $theme_name => $theme_info) {
      $directory = preg_quote(dirname($theme_info->filename), '/');
      $system_tokens[$theme_name] = $directory;
      // Add theme engine if not already set.
      if (!isset($system_tokens[$theme_info->engine])) {
        // Theme engines aren't in the database so we use the information from the
        // theme itself. Using drupal_get_path() would find the path but generate
        // a warning.
        $directory = '';
        if (drupal_realpath($theme_info->owner)) {
          $directory = preg_quote(dirname($theme_info->owner), '/');
        }
        $system_tokens[$theme_info->engine] = $directory;
      }
    }
    // Get install profiles.
    // Same as in install_find_profiles().
    $profiles = file_scan_directory('./profiles', '/\.profile$/', array('key' => 'name'), 0, TRUE);
    foreach ($profiles as $profile => $profile_info) {
      // Add theme engine.
      $directory = preg_quote(drupal_get_path('profile', $profile), '/');
      $system_tokens[$profile] = $directory;
    }
    cache_set("omega:omega_assets_prepare_regex:tokens", $system_tokens, 'cache', CACHE_TEMPORARY);
  }
  else {
    $system_tokens = $cache->data;
  }
  $tokens += $system_tokens;

  foreach ($paths as &$item) {
    // The first segment (everything before the first slash) is the namespace.
    // This rule only applies to local files... So if the namespace can not be
    // mapped to a token, module, profile or theme engine we assume that we are
    // trying to target an external file.
    list($namespace) = explode('/', $item, 2);

    // If a namespace is know but doesn't have a path
    // skip the whole item.
    if (isset($tokens[$namespace]) && empty($tokens[$namespace])) {
      unset($item);
      continue;
    }
    // Handle non-wildcard namespaces that are unknown yet with libraries if
    // available.
    // @TODO should this be part of the $system_tokens cache?
    elseif ($namespace != '*' && !isset($tokens[$namespace]) && function_exists('libraries_get_path')) {
      if ($directory = libraries_get_path($namespace)) {
        $tokens[$namespace] = preg_quote($directory, '/');
      }
    }

    // Escape any regex characters and replace tokens and wildcards.
    $item = isset($tokens[$namespace]) ? substr($item, strlen($namespace)) : $item;
    $item = preg_quote($item, '/');
    $item = str_replace('\*', '.*', $item);
    $item = isset($tokens[$namespace]) ? $tokens[$namespace] . $item : $item;
  }

  return empty($paths) ? FALSE : '/^' . implode('|', $paths) . '$/';
}

/**
 * Groups include/exclude patterns together.
 *
 * This is required to allow for complex chained includes/excludes. We can't do
 * this in a single, complicated regular expression as regex does not really
 * support this (negative lookbehinds are not sufficient).
 *
 * @param array $items
 *   An array of include/exclude patterns.
 *
 * @return array
 *   The include/exclude patterns as groups.
 */
function omega_assets_regex_steps(array $items) {
  $switch = FALSE;
  $key = 0;
  $groups = array();

  // Iterate over all exclude/include paths and form groups. We start a new
  // group every time we switch from inclusion to exclusion.
  foreach ($items as $item) {
    // If an item starts with a "~" it is an inclusion pattern.
    if ($exclude = (strpos($item, '~') === 0)) {
      $item = substr($item, 1);
    }

    // Start a new group if we are switching between exclusion/inclusion.
    if ($switch !== $exclude && ($switch = !$switch) === FALSE) {
      $key++;
    }

    // Initialize the group with two empty arrays if it does not exist yet.
    if (!isset($groups[$key])) {
      $groups[$key] = array_fill(0, 2, array());
    }

    $groups[$key][(!$switch ? 0 : 1)][] = $item;
  }

  // Each group now needs to have its items converted to a regex string.
  foreach ($groups as &$group) {
    foreach (array(0, 1) as $key) {
      $group[$key] = !empty($group[$key]) ? omega_assets_prepare_regex($group[$key]) : array();
    }
  }

  return $groups;
}

/**
 * Eliminates elements from an array using a simplified regex pattern.
 *
 * @param array $mapping
 *   The array of elements that should have some of its items removed.
 * @param array $steps
 *   A set of regex as generated by omega_assets_prepare_regex().
 *
 * @return array
 */
function omega_assets_regex_execute(array $mapping, array $steps) {
  $output = array();
  foreach ($steps as $step) {
    list($exclude, $include) = $step;

    if (!empty($exclude)) {
      $output += preg_grep($exclude, (!empty($output) ? array_diff_key($mapping, $output) : $mapping));
    }

    if (!empty($output) && !empty($include)) {
      $output = array_diff_key($output, preg_grep($include, $output));
    }
  }

  return $output;
}

/**
 * Helper function for generating a map of assets based on the data attribute.
 *
 * We can not rely on the array keys of the JS and CSS file arrays in Drupal
 * because in case of inline JS or CSS (which uses numerical array keys) and due
 * to potential overrides of the 'data' attribute which holds the actual,
 * reliable path of the file. This function returns a single-level array of
 * reliable JS/CSS file paths using the original array keys as keys. Elements of
 * type 'inline' or 'setting' are ignored.
 *
 * @param array $elements
 *   An array of JS or CSS files as given in hook_css_alter() or
 *   hook_js_alter().
 *
 * @return array
 *   A map of file paths generated from $elements.
 *
 * @see hook_js_alter()
 * @see hook_css_alter()
 */
function omega_assets_generate_mapping(array $elements) {
  $mapping = array();
  foreach ($elements as $key => $item) {
    if ($item['type'] == 'inline' || $item['type'] == 'setting') {
      // Naturally, in-line CSS is not supported.
      continue;
    }

    // We need to build an array containing just the 'data' attribute because
    // that's the actual path of the file. The array key of the elements can
    // be something else if someone is sneaky enough to use drupal_add_js() or
    // drupal_add_css() with a bogus first argument (normally, that is the
    // path to the file) and then specify the actual path through the 'data'
    // attribute in the $options array.
    $mapping[$key] = $item['data'];
  }

  return $mapping;
}
