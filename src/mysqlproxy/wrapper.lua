assert(package.loadlib(os.getenv("EDBDIR").."/libexecute.so",
                       "lua_cryptdb_init"))()

--
-- Interception points provided by mysqlproxy
--

function read_auth()
    -- Use this instead of connect_server(), to get server name
    dprint("Connected " .. proxy.connection.client.src.name)
    CryptDB.connect(proxy.connection.client.src.name,
                    proxy.connection.server.dst.address,
                    proxy.connection.server.dst.port,
                    os.getenv("CRYPTDB_USER") or "root",
                    os.getenv("CRYPTDB_PASS") or "letmein",
                    os.getenv("CRYPTDB_DB") or "cryptdbtest")
    -- EDBClient uses its own connection to the SQL server to set up UDFs
    -- and to manipulate multi-principal state.  (And, in the future, to
    -- store its schema state for single- and multi-principal operation.)
end

function disconnect_client()
    dprint("Disconnected " .. proxy.connection.client.src.name)
    CryptDB.disconnect(proxy.connection.client.src.name)
end

function read_query(packet)
    local status, err = pcall(read_query_real, packet)
    if status then
        return err
    else
        print("read_query: " .. err)
        return proxy.PROXY_SEND_QUERY
    end
end

function read_query_result(inj)
    local status, err = pcall(read_query_result_real, inj)
    if status then
        return err
    else
        print("read_query_result: " .. err)
        return proxy.PROXY_SEND_RESULT
    end
end

--
-- Helper functions
--

RES_IGNORE  = 1
RES_DECRYPT = 2

function dprint(x)
    if os.getenv("CRYPTDB_PROXY_DEBUG") then
        print(x)
    end
end

function read_query_real(packet)
    if string.byte(packet) == proxy.COM_QUERY then
        local query = string.sub(packet, 2)
        dprint("read_query: " .. query)

        new_queries = CryptDB.rewrite(proxy.connection.client.src.name, query)
        if #new_queries > 0 then
            for i, v in pairs(new_queries) do
                dprint("new queries: "..v)
                local result_key
                if i == #new_queries then
                    result_key = RES_DECRYPT
                else
                    result_key = RES_IGNORE
                end

                proxy.queries:append(result_key,
                                     string.char(proxy.COM_QUERY) .. v,
                                     { resultset_is_needed = true })
            end

            return proxy.PROXY_SEND_QUERY
        else
            proxy.response.affected_rows = 0
            proxy.response.insert_id = 0
            proxy.response.type = proxy.MYSQLD_PACKET_OK
            return proxy.PROXY_SEND_RESULT
        end
    elseif string.byte(packet) == proxy.COM_QUIT then
        -- do nothing
    else
        print("unexpected packet type " .. string.byte(packet))
    end
end

function read_query_result_real(inj)
    if inj.id == RES_IGNORE then
        return proxy.PROXY_IGNORE_RESULT
    elseif inj.id == RES_DECRYPT then
        local query = inj.query:sub(2)

        -- mysqlproxy doesn't return real lua arrays, so re-package them..
        local fields = {}
        for i = 1, #inj.resultset.fields do
            table.insert(fields, { type = inj.resultset.fields[i].type,
                                   name = inj.resultset.fields[i].name })
        end

        local rows = {}
        if inj.resultset.rows then
            for row in inj.resultset.rows do
                local lrow = {}
                for i = 1, #row do
                    table.insert(lrow, row[i])
                end
                table.insert(rows, lrow)
            end
        end

        dfields, drows = CryptDB.decrypt(proxy.connection.client.src.name,
                                         fields, rows)

        if #dfields > 0 then
            proxy.response.resultset = { fields = dfields, rows = drows }
        end

        proxy.response.affected_rows = inj.resultset.affected_rows
        proxy.response.insert_id = inj.resultset.insert_id
        proxy.response.type = proxy.MYSQLD_PACKET_OK
        return proxy.PROXY_SEND_RESULT
    else
        print("unexpected inj.id " .. inj.id)
    end
end
