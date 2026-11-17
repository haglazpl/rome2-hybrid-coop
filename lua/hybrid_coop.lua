--[[ Rome II hybrid co-op: in-game status display (version 5)
Reads the status written by the Hybrid Co-op DLL (hybrid_coop.dll) from
  %LOCALAPPDATA%\RomeHybridCoop\status.txt
and shows it on the campaign HUD (End Turn button text).
Also writes a one-time dump of the campaign UI tree to
  %LOCALAPPDATA%\RomeHybridCoop\ui_dump.txt
so the display can be moved to a better place later.
Only UI is touched - nothing here can affect the game simulation (no desync risk).
--]]

local M = {}

local root = nil
local dumped = false
local last_line = nil
local targets = nil          -- list of {uic=..., original=...}
local TIMER = "hybrid_coop_status"
local UIC = nil              -- UIComponent constructor, resolved at runtime
local use_game_timer = false

local function episodic()
	return package.loaded["lua_scripts.EpisodicScripting"] or package.loaded["lua_scripts.episodicscripting"]
		or package.loaded["data.lua_scripts.EpisodicScripting"]
end

local function resolve_uic()
	if UIC then return UIC end
	local cands = {}
	pcall(function() cands[#cands + 1] = { "env", UIComponent } end)
	pcall(function() cands[#cands + 1] = { "_G", rawget(_G, "UIComponent") } end)
	pcall(function() cands[#cands + 1] = { "fenv0", getfenv(0).UIComponent } end)
	pcall(function()
		local es = episodic()
		if es then
			cands[#cands + 1] = { "episodic", es.UIComponent }
			for _, fn in pairs({ es.OnUICreated, es.HideComponent, es.EnableComponent }) do
				if type(fn) == "function" then cands[#cands + 1] = { "episodic_env", getfenv(fn).UIComponent } end
			end
			if es.m_root ~= nil then cands[#cands + 1] = { "m_root_metatable", getmetatable(es.m_root) } end
		end
	end)
	for _, c in ipairs(cands) do
		if c[2] ~= nil then UIC = c[2]; return UIC, c[1] end
	end
	return nil
end

local function base_dir()
	local ok, base = pcall(os.getenv, "LOCALAPPDATA")
	if not ok or not base then return nil end
	return base .. "\\RomeHybridCoop"
end

local function log(text)
	pcall(function()
		if out and out.ting then out.ting("HYBRID_COOP: " .. tostring(text)) end
	end)
	local line = tostring(text)
	pcall(function() line = os.date("%H:%M:%S ") .. line end)
	local wrote = false
	pcall(function()
		local d = base_dir()
		if not d then return end
		local f = io.open(d .. "\\lua_log.txt", "a")
		if f then f:write(line .. "\n"); f:close(); wrote = true end
	end)
	if not wrote then   -- fallback: next to the game exe
		pcall(function()
			local f = io.open("hybrid_coop_lua_log.txt", "a")
			if f then f:write(line .. "\n"); f:close() end
		end)
	end
end

local function read_status()
	local d = base_dir()
	if not d then return nil end
	local f = io.open(d .. "\\status.txt", "r")
	if not f then return nil end
	local line = f:read("*l")
	f:close()
	return line
end

local function uic_text(uic)
	local ok, t = pcall(function() return uic:GetStateText() end)
	if ok then return t end
	return nil
end

local function dump_tree()
	if dumped or not root then return end
	dumped = true
	local d = base_dir()
	if not d then return end
	local f = io.open(d .. "\\ui_dump.txt", "w")
	if not f then log("cannot write ui_dump.txt"); return end
	local count = 0
	local function walk(uic, depth)
		if count > 6000 or depth > 25 then return end
		count = count + 1
		local id = "?"
		pcall(function() id = uic:Id() end)
		local text = uic_text(uic)
		local vis = ""
		pcall(function() vis = uic:Visible() and "" or " (hidden)" end)
		f:write(string.rep("  ", depth) .. tostring(id) .. vis .. (text and text ~= "" and (" = [" .. tostring(text) .. "]") or "") .. "\n")
		local n = 0
		pcall(function() n = uic:ChildCount() end)
		for i = 0, n - 1 do
			local ok, child = pcall(function() return UIC(uic:Find(i)) end)
			if ok and child then walk(child, depth + 1) end
		end
	end
	pcall(walk, root, 0)
	f:close()
	log("ui_dump.txt written (" .. count .. " components)")
end

-- the End Turn button's text is its child 'dy_txt' (ui_dump: radar_bottom > end_turn_docker > button_end_turn > dy_txt)
local function find_targets()
	targets = {}
	pcall(function()
		local btn = root:Find("button_end_turn")
		if not btn then return end
		local t = UIC(btn):Find("dy_txt")
		if not t then return end
		local uic = UIC(t)
		table.insert(targets, { uic = uic, name = "button_end_turn/dy_txt", original = uic_text(uic) })
		log("display target: button_end_turn/dy_txt text=" .. tostring(uic_text(uic)))
	end)
	if #targets == 0 then log("no display target found") end
end

-- status line from the probe: stage=shared;you=0;friend=1;diplo=
local function parse(line)
	local st = {}
	for k, v in string.gmatch(line or "", "(%w+)=([^;]*)") do st[k] = v end
	return st
end

-- texts in the game's language (detected from the original End Turn text)
local TEXTS = {
	pl = { ai = "Ruch SI...", diplo = "Sojusznik negocjuje...", both = "Koniec rundy...",
	       you = "Czekam na sojusznika...", friend = "Koniec tury (sojusznik gotowy)" },
	en = { ai = "AI turn...", diplo = "Ally negotiating...", both = "Ending round...",
	       you = "Waiting for ally...", friend = "End Turn (ally ready)" },
}
local function lang()
	local o = targets and targets[1] and targets[1].original or ""
	if string.find(o, "Koniec") then return TEXTS.pl end
	return TEXTS.en
end

local function text_for(st)
	local T = lang()
	if st.stage == "ai" then return T.ai end
	if st.stage ~= "shared" then return nil end
	if st.diplo == "friend" then return T.diplo end
	if st.you == "1" and st.friend == "1" then return T.both end
	if st.you == "1" then return T.you end
	if st.friend == "1" then return T.friend end
	return nil   -- nobody ready: keep the normal button text
end

local function apply(line)
	if not root then return end
	if targets == nil then find_targets() end
	local txt = text_for(parse(line))
	for _, t in ipairs(targets) do
		pcall(function()
			local want = txt or t.original
			if want and uic_text(t.uic) ~= want then t.uic:SetStateText(want) end
		end)
	end
end

local function keep_diplomacy_button(st)
	if not root then return end
	if st.stage ~= "shared" or st.diplo == "friend" or st.you == "1" then return end
	pcall(function()
		local addr = root:Find("button_diplomacy")
		if addr then UIC(addr):SetState("active") end
	end)
end

local pending_component = nil
local function ensure_root()
	if root then return true end
	if not pending_component then return false end
	local f, where = resolve_uic()
	if not f then return false end
	local ok, err = pcall(function() root = f(pending_component) end)
	if not ok then log("root error: " .. tostring(err)); return false end
	log("campaign UI ready (UIComponent from " .. tostring(where) .. ")")
	targets = nil; last_line = nil
	local ok2, err2 = pcall(dump_tree)
	if not ok2 then log("dump error: " .. tostring(err2)) end
	return true
end

local function arm_timer()
	if use_game_timer then
		local ok, err = pcall(function() episodic().game_interface:add_time_trigger(TIMER, 0.5) end)
		if not ok then log("game timer error: " .. tostring(err)) end
	end
end

local function tick()
	ensure_root()
	local line = read_status() or "stage=off"
	pcall(keep_diplomacy_button, parse(line))
	if line ~= last_line then
		last_line = line
		log("status: " .. line)
	end
	apply(line)
end

function M.attach(ev)
	if M.attached then return end
	M.attached = true
	log("attached: io=" .. type(io) .. " os=" .. type(os) .. " Timers=" .. type(Timers) .. " UIComponent=" .. type(UIComponent))
	table.insert(ev.UICreated, function(context)
		log("UICreated: " .. tostring(context.string))
		if context.string ~= "Campaign UI" then return end
		root = nil
		pending_component = context.component
		pcall(function() local f = io.open(base_dir() .. "\\ui_state.txt", "w"); if f then f:write("diplomacy=0\n"); f:close() end end)
		local ok, err = pcall(ensure_root)
		if not ok then log("ensure_root error: " .. tostring(err)) end
		local okT, errT = pcall(function() Timers.register_repeating(TIMER, 500) end)
		if okT then
			log("timer: Timers.register_repeating")
		else
			log("Timers unavailable (" .. tostring(errT) .. "), using game_interface:add_time_trigger")
			use_game_timer = true
			arm_timer()
		end
	end)
	local open_panels = {}
	local function write_ui_state()
		pcall(function()
			local diplomacy = 0
			for name, _ in pairs(open_panels) do
				if string.find(string.lower(name), "diplomacy") then diplomacy = 1 end
			end
			local f = io.open(base_dir() .. "\\ui_state.txt", "w")
			if f then f:write("diplomacy=" .. diplomacy .. "\n"); f:close() end
		end)
	end
	pcall(function()
		table.insert(ev.PanelOpenedCampaign, function(context)
			local name = tostring(context.string)
			open_panels[name] = true
			log("panel opened: " .. name)
			write_ui_state()
		end)
		table.insert(ev.PanelClosedCampaign, function(context)
			local name = tostring(context.string)
			open_panels[name] = nil
			log("panel closed: " .. name)
			write_ui_state()
		end)
	end)
	table.insert(ev.TimeTrigger, function(context)
		if context.string == TIMER then
			local ok, err = pcall(tick)
			if not ok then log("tick error: " .. tostring(err)) end
			arm_timer()
		end
	end)
end

return M
