const EXCLUSIVE_SHORT_PRESS_ACTIONS = new Set(["page_enter", "page_back"]);

export function assignButtonAction(controlRows, buttonActions, changedRow, actionId) {
  const nextActions = { ...(buttonActions || {}) };
  if (EXCLUSIVE_SHORT_PRESS_ACTIONS.has(actionId)
      && String(changedRow?.event || "").endsWith(".short_press")) {
    controlRows.forEach((row) => {
      if (row.id !== changedRow.id
          && String(row.event || "").endsWith(".short_press")
          && nextActions[row.id] === actionId) {
        nextActions[row.id] = "disabled";
      }
    });
  }
  nextActions[changedRow.id] = actionId;
  return nextActions;
}
