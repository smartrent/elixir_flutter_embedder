defmodule FlutterEmbedder.MDNSClient do
  @moduledoc "Simple MDNS Client to discover networked Nerves devices"
  use GenServer
  require Logger
  @mdns_group {224, 0, 0, 251}
  @mdns_port 5353

  defmodule State do
    defstruct mdns_socket: nil, discovered: []
  end

  @query_packet %DNS.Record{
    header: %DNS.Header{},
    qdlist: []
  }

  def start_link(args) do
    GenServer.start_link(__MODULE__, args, name: __MODULE__)
  end

  def discover(pid) do
    query(pid, :ptr, '_dartobservatory._tcp.local')
  end

  def query(client, type, domain) do
    GenServer.call(client, {:query, type, domain})
  end

  @impl GenServer
  def init(_args) do
    send(self(), :open_mdns)
    {:ok, %State{}}
  end

  @impl GenServer
  def handle_call({:query, type, query}, from, state) do
    send_query(state.mdns_socket, type, query)
    Process.send_after(self(), {:query_result, from}, 1500)
    {:noreply, state}
  end

  @impl GenServer
  def handle_info({:query_result, from}, state) do
    results = Enum.map(state.discovered, fn {_ref, data} -> data end)
    GenServer.reply(from, {:ok, results})
    {:noreply, state}
  end

  def handle_info({:ttl, ref}, state) do
    discovered =
      Enum.reject(state.discovered, fn
        {^ref, _} -> true
        _ -> false
      end)

    {:noreply, %{state | discovered: discovered}}
  end

  def handle_info(:open_mdns, state) do
    udp_options = [
      :binary,
      broadcast: true,
      active: true,
      ip: {0, 0, 0, 0},
      ifaddr: {0, 0, 0, 0},
      add_membership: {@mdns_group, {0, 0, 0, 0}},
      multicast_loop: true,
      multicast_ttl: 32,
      reuseaddr: true
    ]

    case :gen_udp.open(0, udp_options) do
      {:ok, socket} ->
        {:noreply, %State{state | mdns_socket: socket}}

      error ->
        {:stop, {:multicast, error}, state}
    end
  end

  def handle_info({:udp, socket, ip, _port, packet}, %{mdns_socket: socket} = state) do
    record = DNS.Record.decode(packet)
    state = handle_mdns(record.anlist, ip, state)
    {:noreply, state}
  end

  # i'm so sorry about this. It's not the correct way to do this.
  # I don't think an MDNS client is the solution to this long term
  # since it won't work on macos
  defp handle_mdns([%{type: :txt, domain: domain, data: data, ttl: ttl} | rest], ip, state) do
    state =
      case String.split(to_string(domain), ".") do
        [host, "_dartobservatory", "_tcp", "local"] ->
          case URI.decode_query(to_string(data)) do
            %{"port" => port, "path" => path} ->
              port = String.to_integer(port)
              uri = %URI{scheme: "http", host: "#{host}.local", port: port, path: path}

              duplicate_uri =
                Enum.find(state.discovered, fn
                  {_, ^uri} -> true
                  _ -> false
                end)

              if duplicate_uri do
                state
              else
                ref = make_ref()
                Process.send_after(self(), {:ttl, ref}, ttl * 1000)
                %{state | discovered: [{ref, uri} | state.discovered]}
              end

            _ ->
              state
          end

        _ ->
          state
      end

    handle_mdns(rest, ip, state)
  end

  defp handle_mdns([_unknown | rest], ip, state) do
    handle_mdns(rest, ip, state)
  end

  defp handle_mdns([], _ip, state) do
    state
  end

  defp send_query(socket, type, domain) do
    packet = %DNS.Record{
      @query_packet
      | :qdlist => [
          %DNS.Query{domain: domain, type: type, class: :in}
        ]
    }

    p = DNS.Record.encode(packet)
    :ok = :gen_udp.send(socket, @mdns_group, @mdns_port, p)
  end
end
