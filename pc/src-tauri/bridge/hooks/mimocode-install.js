"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");

const MANAGED_PLUGIN_FILE_NAME = "pet-manager.js";
const OPENCODE_PLUGIN_FILE_NAME = "opencode-pet-manager.js";

function resolveMiMoCodeConfigDir(options = {}) {
  const env = options.env || process.env;
  const home = options.home || env.HOME || env.USERPROFILE || os.homedir();
  if (env.MIMOCODE_CONFIG_DIR) return path.resolve(env.MIMOCODE_CONFIG_DIR);
  if (env.XDG_CONFIG_HOME) return path.join(path.resolve(env.XDG_CONFIG_HOME), "mimocode");
  return path.join(path.resolve(home), ".config", "mimocode");
}

function resolveOpenCodeConfigDir(options = {}) {
  const env = options.env || process.env;
  const home = options.home || env.HOME || env.USERPROFILE || os.homedir();
  if (env.OPENCODE_CONFIG_DIR) return path.resolve(env.OPENCODE_CONFIG_DIR);
  if (env.XDG_CONFIG_HOME) return path.join(path.resolve(env.XDG_CONFIG_HOME), "opencode");
  return path.join(path.resolve(home), ".config", "opencode");
}

function syncMiMoCodePlugin(options = {}) {
  const sourcePath = options.sourcePath
    || path.join(__dirname, "mimocode-pet-manager-plugin.js");
  const configDir = options.configDir || resolveMiMoCodeConfigDir(options);
  const pluginDir = path.join(configDir, options.pluginDirName || "plugin");
  const fileName = options.fileName || MANAGED_PLUGIN_FILE_NAME;
  const targetPath = path.join(pluginDir, fileName);
  const source = fs.readFileSync(sourcePath);
  let previous = null;
  try {
    previous = fs.readFileSync(targetPath);
  } catch {}

  if (previous && Buffer.compare(previous, source) === 0) {
    return {
      added: 0,
      updated: 0,
      removed: 0,
      skipped: 1,
      targetPath,
    };
  }

  fs.mkdirSync(pluginDir, { recursive: true });
  const tempPath = path.join(
    pluginDir,
    `.${fileName}.${process.pid}.${Date.now()}.tmp`,
  );
  fs.writeFileSync(tempPath, source);
  try {
    fs.renameSync(tempPath, targetPath);
  } catch (error) {
    try { fs.unlinkSync(targetPath); } catch {}
    try {
      fs.renameSync(tempPath, targetPath);
    } catch {
      try { fs.unlinkSync(tempPath); } catch {}
      throw error;
    }
  }

  return {
    added: previous ? 0 : 1,
    updated: previous ? 1 : 0,
    removed: 0,
    skipped: 0,
    targetPath,
  };
}

function syncOpenCodePlugin(options = {}) {
  const configDir = options.configDir || resolveOpenCodeConfigDir(options);
  const result = syncMiMoCodePlugin({
    ...options,
    configDir,
    pluginDirName: "plugins",
    fileName: OPENCODE_PLUGIN_FILE_NAME,
  });
  const obsoletePath = path.join(configDir, "plugin", MANAGED_PLUGIN_FILE_NAME);
  try {
    fs.unlinkSync(obsoletePath);
    result.removed += 1;
  } catch (error) {
    if (error.code !== "ENOENT") throw error;
  }
  return result;
}

module.exports = {
  MANAGED_PLUGIN_FILE_NAME,
  OPENCODE_PLUGIN_FILE_NAME,
  resolveMiMoCodeConfigDir,
  resolveOpenCodeConfigDir,
  syncMiMoCodePlugin,
  syncOpenCodePlugin,
};
