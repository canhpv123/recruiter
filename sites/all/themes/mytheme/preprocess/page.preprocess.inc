<?php

/**
 * Implements hook_preprocess_page().
 */
function mytheme_preprocess_page(&$variables) {
  if (drupal_is_front_page()) {
    // Remove the main content block on the front page (empty anyways).
    unset($variables['page']['content']['system_main']);
  }
}