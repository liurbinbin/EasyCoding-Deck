// 动态界面模板只替换固定文案；数字、Agent 名、SSID、文件名和请求标识原样保留。
export function translateControlLabel(value) {
  const label = String(value).trim();
  const parts = label.split("或");
  if (parts.length > 1) {
    const translated = parts.map(translateControlLabel);
    return translated.every(Boolean) ? translated.join(" or ") : null;
  }
  const direct = {
    "按下摇杆": "press the joystick",
    "长按摇杆中键": "hold the joystick center",
    "摇杆向上": "move the joystick up",
    "摇杆向下": "move the joystick down",
    "摇杆向左": "move the joystick left",
    "摇杆向右": "move the joystick right",
    "前方旋钮短按": "press the front dial",
    "前方旋钮长按": "hold the front dial",
    "前方旋钮旋转": "rotate the front dial",
    "摇杆": "Joystick",
  }[label];
  if (direct) return direct;
  let match = label.match(/^按键\s*(\d+)\s*(短按|长按)?$/);
  if (match) return match[2] ? `${match[2] === "长按" ? "hold" : "press"} Key ${match[1]}` : `Key ${match[1]}`;
  match = label.match(/^(短按|长按|按住)\s*(\d+)\s*键$/);
  if (match) return `${match[1] === "短按" ? "press" : "hold"} Key ${match[2]}`;
  match = label.match(/^(SW\d+)\s*(短按|长按)$/);
  return match ? `${match[2] === "短按" ? "press" : "hold"} ${match[1]}` : null;
}

const templates = [
  [/^(.+) · (.+) · ([\d.]+) 秒(?: \/ 上限 ([\d.]+) 秒)?((?: · HTTP \d+)?) · 请求编号：(.+?)(?: · 本地追踪：(.+))?$/, (m, t) => `${t(m[1])} · ${t(m[2])} · ${m[3]} seconds${m[4] ? ` / limit ${m[4]} seconds` : ""}${m[5]} · Request ID: ${t(m[6])}${m[7] ? ` · Local trace: ${m[7]}` : ""}`],
  [/^(豆包|火山引擎|可灵 AI) · (.+)$/, (m, t) => `${t(m[1])} · ${m[2]}`],
  [/^已检测到 (\d+) 个 Agent，设备端仅显示当前跟随的 Agent 状态，对话结束后设备气泡保留显示 60 秒$/, m => `${m[1]} Agent(s) detected. The device shows only the followed Agent's status; conversation bubbles remain for 60 seconds after completion.`],
  [/^(\d+) 块 (.+)$/, m => `${m[1]} ${m[2]} displays`],
  [/^(\d+) 个带屏按键 · 非触摸$/, m => `${m[1]} screen keys · no touch input`],
  [/^(\d+) 灯 · QWIIC$/, m => `${m[1]} LEDs · QWIIC`],
  [/^(\d+)% · 充电中$/, m => `${m[1]}% · Charging`],
  [/^第 (\d+) 块屏幕下方( \/ 电源键)?$/, m => `Below screen ${m[1]}${m[2] ? " / power key" : ""}`],
  [/^按键 (\d+) · 第 (\d+) 块屏幕下方( \/ 电源键)?$/, m => `Key ${m[1]} · Below screen ${m[2]}${m[3] ? " / power key" : ""}`],
  [/^(\d+) (秒|分钟)$/, m => `${m[1]} ${m[2] === "秒" ? "seconds" : "minutes"}`],
  [/^(\d+) 个控件\s*(?:·)?\s*(\d+) 个手势$/, m => `${m[1]} controls · ${m[2]} gestures`],
  [/^(\d+) \/ (\d+) 项凭据已配置$/, m => `${m[1]} / ${m[2]} credentials configured`],
  [/^发现 (\d+) 个 Wi-Fi 网络$/, m => `Found ${m[1]} Wi-Fi network(s)`],
  [/^检测到 (\d+) 个本机 Agent。$/, m => `Detected ${m[1]} local Agent(s).`],
  [/^进行中 · (.+)$/, (m, t) => `In progress · ${t(m[1])}`],
  [/^(.+)就绪$/, m => `${m[1].trim()} ready`],
  [/^(.+)说话，松开后将文字追加到 Agent 输入框；可以分多次说。$/, m => {
    const control = translateControlLabel(m[1]);
    return control ? `${control} to speak. Release to append text to the Agent composer; repeat to add more.` : null;
  }],
  [/^(.+)确认发送。$/, m => {
    const control = translateControlLabel(m[1]);
    return control ? `${control} to confirm and send.` : null;
  }],
  [/^在宠物界面，(.+)开始聊天；再次执行相同操作结束。$/, m => {
    const control = translateControlLabel(m[1]);
    return control ? `On the pet screen, ${control} to start a conversation. Repeat to end it.` : null;
  }],
  [/^(.+)退出组件，返回组件列表。$/, m => {
    const control = translateControlLabel(m[1]);
    return control ? `${control} to exit to the component list.` : null;
  }],
  [/^(.+)打开选中的组件。$/, m => {
    const control = translateControlLabel(m[1]);
    return control ? `${control} to open the selected component.` : null;
  }],
  [/^(.+)选择(上一个|下一个)$/, m => {
    const control = translateControlLabel(m[1]);
    return control ? `${control} to select the ${m[2] === "上一个" ? "previous" : "next"} item` : null;
  }],
  [/^(.+)，在宠物界面与组件列表之间切换。组件运行时请先返回。$/, m => {
    const control = translateControlLabel(m[1]);
    return control ? `${control} to switch between the pet screen and component list. Exit a running component first.` : null;
  }],
  [/^在宠物界面，使用「上一个／下一个」切换会话气泡。(.+)。$/, m => {
    const controls = m[1].split("；").map(translateControlLabel);
    return controls.every(Boolean) ? `On the pet screen, use Previous / Next to browse session bubbles: ${controls.join("; ")}.` : null;
  }],
  [/^请先配置(语音服务和对话大模型|语音服务|对话大模型)。$/, m => `Configure ${m[1] === "语音服务" ? "speech services" : m[1] === "对话大模型" ? "a conversation model" : "speech services and a conversation model"} first.`],
  [/^(.+) 凭据已从统一配置读取$/, m => `${m[1]} credentials loaded from shared settings`],
  [/^请先在 API 配置中补充 (.+)$/, m => `First add ${m[1]} in API settings`],
  [/^已保存 (.+)；留空保持不变$/, m => `Saved ${m[1]}; leave blank to keep unchanged`],
  [/^输入(.+) API Key$/, m => `Enter the ${m[1]} API key`],
  [/^Key 已保存，语音识别测试通过（(.+) ms）。可到「人设与声音」试听，检查语音合成是否可用。$/, m => `Key saved. Speech recognition test passed (${m[1]} ms). Preview the voice in Persona & Voice to check speech synthesis.`],
  [/^已准备 (.+) · (.+) 秒$/, m => `Ready: ${m[1]} · ${m[2]} seconds`],
  [/^已选择内置音效：(.+) · (.+) 秒$/, (m, t) => `Built-in sound: ${t(m[1])} · ${m[2]} seconds`],
  [/^播放完成 · (.+) 秒$/, m => `Playback complete · ${m[1]} seconds`],
  [/^内置：(.+)$/, (m, t) => `Built-in: ${t(m[1])}`],
  [/^(.+) 个动画可用 · (.+)$/, m => `${m[1]} animations available · ${m[2]}`],
  [/^(.+) 个已有素材$/, m => `${m[1]} existing assets`],
  [/^已处理 (\d+)\/(\d+)$/, m => `Processed ${m[1]}/${m[2]}`],
  [/^已保存 (\d+)\/(\d+) 段$/, m => `Saved ${m[1]}/${m[2]} clips`],
  [/^(.+) 个新增或更新的形象$/, m => `${m[1]} new or updated appearances`],
  [/^搜索音色 · 共 (\d+) 种$/, m => `Search voices · ${m[1]} total`],
  [/^(.+)（需要长按）$/, (m, t) => `${t(m[1])} (long press required)`],
  [/^常用操作 · 按当前配置显示（待同步）$/, () => "Quick reference · current mappings (pending sync)"],
  [/^生成历史（(\d+)）$/, m => `Generation history (${m[1]})`],
  [/^(.+)（待同步）$/, (m, t) => `${t(m[1])} (pending sync)`],
  [/^「(.+)」(部分生成完成|生成完成|生成失败|部分已保存，后续未完成)$/, (m, t) => `“${m[1]}” · ${t(m[2])}`],
  [/^(等待确认|正在生成)「(.+)」$/, (m, t) => `${t(m[1])} “${m[2]}”`],
  [/^(.+)（未开通或已失效）$/, m => `${m[1]} (not enabled or expired)`],
];

export function translateDynamicCopy(value, translate) {
  const control = translateControlLabel(value);
  if (control) return control;
  for (const [pattern, format] of templates) {
    const match = value.match(pattern);
    if (!match) continue;
    const translated = format(match, translate);
    if (translated) return translated;
  }
  return null;
}
