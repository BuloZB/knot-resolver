-- Fetch over HTTPS with peert cert checked
local function https_fetch(url, ca)
	local ssl_ok, https = pcall(require, 'ssl.https')
	local ltn_ok, ltn12 = pcall(require, 'ltn12')
	if not ssl_ok or not ltn_ok then
		return nil, 'luasec and luasocket needed for root TA bootstrap'
	end
	local resp = {}
	local r, c, h, s = https.request{
	       url = url,
	       cafile = ca,
	       verify = {'peer', 'fail_if_no_peer_cert' },
	       protocol = 'tlsv1_2',
	       sink = ltn12.sink.table(resp),
	}
	if r == nil then return r, c end
	return resp[1]
end

-- Fetch root anchors in XML over HTTPS
local function bootstrap(url, ca)
	-- @todo ICANN certificate is verified against current CA
	--       this is not ideal, as it should rather verify .xml signature which
	--       is signed by ICANN long-lived cert, but luasec has no PKCS7
	ca = ca or etcdir..'/icann-ca.pem'
	url = url or 'https://data.iana.org/root-anchors/root-anchors.xml'
	local xml, err = https_fetch(url, ca)
	if not xml then
		return false, string.format('[ ta ] fetch of "%s" failed: %s', url, err)
	end
	-- Parse root trust anchor
	local fields = {}
	string.gsub(xml, "<([%w]+).->([^<]+)</[%w]+>", function (k, v) fields[k] = v end)
	local rrdata = string.format('%s %s %s %s', fields.KeyDigest, fields.Algorithm, fields.DigestType, fields.Digest)
	local rr = string.format('%s 0 IN DS %s', fields.TrustAnchor, rrdata)
	-- Add to key set, create an empty keyset file to be filled
	print('[ ta ] warning: root anchor bootstrapped, you SHOULD check the key manually, see: '..
	      'https://data.iana.org/root-anchors/draft-icann-dnssec-trust-anchor.html#sigs')
	return rr
end

-- Load the module (check for FFI)
local ffi_ok, ffi = pcall(require, 'ffi')
if not ffi_ok then
	-- Simplified TA management, no RFC5011 automatics
	return {
		-- Reuse Lua/C global function
		add = trustanchor,
		-- Simplified trust anchor management
		config = function (path)
			if not path then return end
			if not io.open(path, 'r') then
				local rr, err = bootstrap()
				if not rr then print(err) return false end
				local keyfile = assert(io.open(path, 'w'))
				keyfile:write(rr..'\n')
			end
			for line in io.lines(path) do
				trustanchor(line)
			end
		end,
		-- Disabled
		set_insecure = function () error('[ ta ] FFI not available, this function is disabled') end,
	}
end
local kres = require('kres')
local C = ffi.C

-- RFC5011 state table
local key_state = {
	Start = 'Start', AddPend = 'AddPend', Valid = 'Valid',
	Missing = 'Missing', Revoked = 'Revoked', Removed = 'Removed'
}

-- Find key in current keyset
local function ta_find(keyset, rr)
	for i, ta in ipairs(keyset) do
		-- Match key owner and content
		if ta.owner == rr.owner then
			if ta.type == rr.type then
				if rr.type == kres.type.DNSKEY then
					if C.kr_dnssec_key_match(ta.rdata, #ta.rdata, rr.rdata, #rr.rdata) == 0 then
						return ta
					end
				elseif rr.type == kres.type.DS and ta.rdata == rr.rdata then
					return ta
				end
			-- DNSKEY superseding DS, inexact match
			elseif rr.type == kres.type.DNSKEY and ta.type == kres.type.DS then
				if ta.key_tag == C.kr_dnssec_key_tag(rr.type, rr.rdata, #rr.rdata) then
					keyset[i] = rr -- Replace current DS
					rr.state = ta.state
					rr.key_tag = ta.key_tag
					return rr
				end
			-- DS key matching DNSKEY, inexact match
			elseif rr.type == kres.type.DS and ta.type == kres.type.DNSKEY then
				local ds_tag = C.kr_dnssec_key_tag(rr.type, rr.rdata, #rr.rdata)
				local dnskey_tag = C.kr_dnssec_key_tag(ta.type, ta.rdata, #ta.rdata)
				if ds_tag == dnskey_tag then
					return ta
				end 
			end
		end
	end
	return nil
end

-- Evaluate TA status according to RFC5011
local function ta_present(keyset, rr, hold_down_time, force)
	if rr.type == kres.type.DNSKEY and not C.kr_dnssec_key_ksk(rr.rdata) then
		return false -- Ignore
	end
	-- Find the key in current key set and check its status
	local now = os.time()
	local key_revoked = (rr.type == kres.type.DNSKEY) and C.kr_dnssec_key_revoked(rr.rdata)
	local key_tag = C.kr_dnssec_key_tag(rr.type, rr.rdata, #rr.rdata)
	local ta = ta_find(keyset, rr)
	if ta then
		-- Key reappears (KeyPres)
		if ta.state == key_state.Missing then
			ta.state = key_state.Valid
			ta.timer = nil
		end
		-- Key is revoked (RevBit)
		if ta.state == key_state.Valid or ta.state == key_state.Missing then
			if key_revoked then
				ta.state = key_state.Revoked
				ta.timer = os.time() + hold_down_time
			end
		end
		-- Remove hold-down timer expires (RemTime)
		if ta.state == key_state.Revoked and os.difftime(ta.timer, now) <= 0 then
			ta.state = key_state.Removed
			ta.timer = nil
		end
		-- Add hold-down timer expires (AddTime)
		if ta.state == key_state.AddPend and os.difftime(ta.timer, now) <= 0 then
			ta.state = key_state.Valid
			ta.timer = nil
		end
		if rr.state ~= key_state.Valid or verbose() then
			print('[ ta ] key: '..key_tag..' state: '..ta.state)
		end
		return true
	elseif not key_revoked then -- First time seen (NewKey)
		rr.key_tag = key_tag
		if force then
			rr.state = key_state.Valid
		else
			rr.state = key_state.AddPend
			rr.timer = now + hold_down_time
		end
		if rr.state ~= key_state.Valid or verbose() then
			print('[ ta ] key: '..key_tag..' state: '..rr.state)
		end
		table.insert(keyset, rr)
		return true
	end
	return false
end

-- TA is missing in the new key set
local function ta_missing(ta, hold_down_time)
	-- Key is removed (KeyRem)
	local keep_ta = true
	local key_tag = C.kr_dnssec_key_tag(ta.type, ta.rdata, #ta.rdata)
	if ta.state == key_state.Valid then
		ta.state = key_state.Missing
		ta.timer = os.time() + hold_down_time
	-- Purge pending key
	elseif ta.state == key_state.AddPend then
		print('[ ta ] key: '..key_tag..' purging')
		keep_ta = false
	end
	print('[ ta ] key: '..key_tag..' state: '..ta.state)
	return keep_ta
end

-- Plan refresh event and re-schedule itself based on the result of the callback
local function refresh_plan(trust_anchors, timeout, refresh_cb, priming, bootstrap)
	trust_anchors.refresh_ev = event.after(timeout, function (ev)
		resolve('.', kres.type.DNSKEY, kres.class.IN, kres.query.NO_CACHE,
		function (pkt)
			-- Schedule itself with updated timeout
			local next_time = refresh_cb(trust_anchors, kres.pkt_t(pkt), bootstrap)
			if trust_anchors.refresh_time ~= nil then
				next_time = trust_anchors.refresh_time
			end
			print('[ ta ] next refresh: '..next_time)
			refresh_plan(trust_anchors, next_time, refresh_cb)
			-- Priming query, prime root NS next
			if priming ~= nil then
				resolve('.', kres.type.NS, kres.class.IN)
			end
		end)
	end)
end

-- Active refresh, return time of the next check
local function active_refresh(trust_anchors, pkt, bootstrap)
	local retry = true
	if pkt:rcode() == kres.rcode.NOERROR then
		local records = pkt:section(kres.section.ANSWER)
		local keyset = {}
		for i, rr in ipairs(records) do
			if rr.type == kres.type.DNSKEY then
				table.insert(keyset, rr)
			end
		end
		trust_anchors.update(keyset, bootstrap)
		retry = false
	end
	-- Calculate refresh/retry timer (RFC 5011, 2.3)
	local min_ttl = retry and day or 15 * day
	for i, rr in ipairs(trust_anchors.keyset) do -- 10 or 50% of the original TTL
		min_ttl = math.min(min_ttl, (retry and 100 or 500) * rr.ttl)
	end
	return math.max(hour, min_ttl)
end

-- Write keyset to a file
local function keyset_write(keyset, path)
	local file = assert(io.open(path..'.lock', 'w'))
	for i = 1, #keyset do
		local ta = keyset[i]
		local rr_str = string.format('%s ; %s\n', kres.rr2str(ta), ta.state)
		if ta.state ~= key_state.Valid and ta.state ~= key_state.Missing then
			rr_str = '; '..rr_str -- Invalidate key string
		end
		file:write(rr_str)
	end
	file:close()
	os.rename(path..'.lock', path)
end

-- TA store management
local trust_anchors = {
	keyset = {},
	insecure = {},
	hold_down_time = 30 * day,
	keep_removed = 0,
	-- Update existing keyset
	update = function (new_keys, initial)
		if not new_keys then return false end
		-- Filter TAs to be purged from the keyset (KeyRem)
		local hold_down = trust_anchors.hold_down_time / 1000
		local keyset = {}
		local keep_removed = trust_anchors.keep_removed
		for i, ta in ipairs(trust_anchors.keyset) do
			local keep = true
			if not ta_find(new_keys, ta) then
				keep = ta_missing(ta, hold_down)
			end
			-- Purge removed keys
			if ta.state == key_state.Removed then
				if keep_removed > 0 then
					keep_removed = keep_removed - 1
				else
					keep = false
				end
			end
			if keep then
				table.insert(keyset, ta)
			end
		end
		-- Evaluate new TAs
		for i, rr in ipairs(new_keys) do
			if (rr.type == kres.type.DNSKEY or rr.type == kres.type.DS) and rr.rdata ~= nil then
				ta_present(keyset, rr, hold_down, initial)
			end
		end
		-- Publish active TAs
		local store = kres.context().trust_anchors
		C.kr_ta_clear(store)
		if next(keyset) == nil then return false end
		for i, ta in ipairs(keyset) do
			-- Key MAY be used as a TA only in these two states (RFC5011, 4.2)
			if ta.state == key_state.Valid or ta.state == key_state.Missing then
				C.kr_ta_add(store, ta.owner, ta.type, ta.ttl, ta.rdata, #ta.rdata)
			end
		end
		trust_anchors.keyset = keyset
		-- Store keyset in the file
		if trust_anchors.file_current ~= nil then
			keyset_write(keyset, trust_anchors.file_current)
		end
		return true
	end,
	-- Load keys from a file (managed)
	config = function (path, unmanaged)
		-- Bootstrap if requested and keyfile doesn't exist
		if trust_anchors.refresh_ev ~= nil then event.cancel(trust_anchors.refresh_ev) end
		if not io.open(path, 'r') then
			local rr, msg = bootstrap()
			if not rr then
				error('you MUST obtain the root TA manually, see: '..
				      'http://knot-resolver.readthedocs.org/en/latest/daemon.html#enabling-dnssec')
			end
			trustanchor(rr)
			-- Fetch DNSKEY immediately
			trust_anchors.file_current = path
			refresh_plan(trust_anchors, 0, active_refresh, true, true)
			return
		elseif path == trust_anchors.file_current then
			return
		end
		-- Parse new keys, refresh eventually
		local new_keys = require('zonefile').file(path)
		if unmanaged then
			trust_anchors.file_current = nil
		else
			trust_anchors.file_current = path
		end
		trust_anchors.keyset = {}
		if trust_anchors.update(new_keys, true) then
			refresh_plan(trust_anchors, 10 * sec, active_refresh, true, false)
		end
	end,
	-- Add DS/DNSKEY record(s) (unmanaged)
	add = function (keystr)
		return trustanchor(keystr)
	end,
	-- Negative TA management
	set_insecure = function (list)
		local store = kres.context().negative_anchors
		C.kr_ta_clear(store)
		for i = 1, #list do
			local dname = kres.str2dname(list[i])
			C.kr_ta_add(store, dname, kres.type.DS, 0, nil, 0)
		end
		trust_anchors.insecure = list
	end,
}

return trust_anchors
