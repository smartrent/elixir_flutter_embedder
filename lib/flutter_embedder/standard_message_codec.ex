defmodule FlutterEmbedder.StandardMessageCodec do
  @kStdNull 0
  @kStdTrue 1
  @kStdFalse 2
  @kStdInt32 3
  @kStdInt64 4
  # @kStdLargeInt 5 # not used?
  @kStdFloat64 6
  @kStdString 7
  @kStdUInt8Array 8
  @kStdInt32Array 9
  @kStdInt64Array 10
  @kStdFloat64Array 11
  @kStdList 12
  @kStdMap 13

  @type dynamic_list() :: [value()]
  @type dart_map() :: map()
  @type int64() :: integer()
  @type int32() :: integer()
  @type float64() :: float()
  @type dart_string() :: binary()
  @type value :: dart_string() | float64() | int32() | int64() | dart_map() | dynamic_list()

  defmodule DecodeError do
    defexception [:message]
  end

  @doc "Checks if a value can be encoded into a Dart value"
  defguard is_valid_dart_value(value)
           when is_binary(value) or
                  is_integer(value) or
                  is_float(value) or
                  is_boolean(value) or
                  is_map(value) or
                  is_list(value)

  @spec encode_value(value()) :: binary()
  def encode_value(nil), do: <<@kStdNull>>
  def encode_value(true), do: <<@kStdTrue>>
  def encode_value(false), do: <<@kStdFalse>>

  def encode_value(int32) when is_integer(int32) and abs(int32) <= 0x7FFFFFFF,
    do: <<@kStdInt32, int32::signed-native-32>>

  def encode_value(int64) when is_integer(int64) and abs(int64) <= 0x7FFFFFFFFFFFFFFF,
    do: <<@kStdInt64, int64::signed-native-64>>

  def encode_value(float64) when is_float(float64),
    do: <<@kStdFloat64, 0::6*8, float64::signed-native-float-64>>

  def encode_value(string) when is_binary(string) and byte_size(string) < 254 do
    <<@kStdString, byte_size(string)::8, string::binary>>
  end

  def encode_value(string) when is_binary(string) and byte_size(string) < 0xFFFF do
    <<@kStdString, 254, byte_size(string)::native-16, string::binary>>
  end

  # TODO encode @kStdUInt8Array, @kStdInt32Array, @kStdInt64Array, @kStdFloat64Array
  def encode_value(value) when is_list(value) do
    acc = <<@kStdList, length(value)::8>>

    Enum.reduce(value, acc, fn
      value, acc when is_valid_dart_value(value) ->
        acc <> encode_value(value)

      _invalid, _acc ->
        raise ArgumentError
    end)
  end

  # i don't think Dart actually allows for maps as return values via PlatformChannel
  def encode_value(%{} = map) do
    acc = <<@kStdMap, map_size(map)::8>>

    Enum.reduce(map, acc, fn
      # Dart only allows string keys
      {key, value}, acc when is_binary(key) and is_valid_dart_value(value) ->
        acc <> encode_value(key) <> encode_value(value)

      {_key, _value}, _acc ->
        raise ArgumentError
    end)
  end

  @spec decode_value(binary()) :: {value(), binary()} | no_return
  def decode_value(<<@kStdMap, num_pairs::8, map::binary>>) do
    decode_map(num_pairs, map, %{})
  end

  def decode_value(<<@kStdList, num_items::8, values::binary>>) do
    decode_dynamic_list(num_items, values, [])
  end

  def decode_value(<<@kStdFloat64Array, num_items, float64_list::binary>>) do
    decode_float64_list(num_items, float64_list, [])
  end

  def decode_value(<<@kStdInt64Array, num_items, uint64_list::binary>>) do
    decode_uint64_list(num_items, uint64_list, [])
  end

  def decode_value(<<@kStdInt32Array, num_items, uint32_list::binary>>) do
    decode_uint32_list(num_items, uint32_list, [])
  end

  def decode_value(<<@kStdUInt8Array, num_items, uint8_list::binary>>) do
    decode_uint8_list(num_items, uint8_list, [])
  end

  def decode_value(
        <<@kStdString, 254, length::native-16, string::binary-size(length), rest::binary>>
      ),
      do: {string, rest}

  def decode_value(<<@kStdString, length::8, string::binary-size(length), rest::binary>>),
    do: {string, rest}

  def decode_value(<<@kStdFloat64, _pad::6*8, float64::signed-native-float-64, rest::binary>>),
    do: {float64, rest}

  def decode_value(<<@kStdInt64, int64::signed-native-64, rest::binary>>), do: {int64, rest}
  def decode_value(<<@kStdInt32, int32::signed-native-32, rest::binary>>), do: {int32, rest}
  def decode_value(<<@kStdFalse, rest::binary>>), do: {false, rest}
  def decode_value(<<@kStdTrue, rest::binary>>), do: {true, rest}
  def decode_value(<<@kStdNull, rest::binary>>), do: {nil, rest}

  def decode_value(<<type, _::binary>>) when type in 0..13 do
    raise DecodeError, message: "Could not decode known type: #{type}"
  end

  def decode_value(<<type, _::binary>>) do
    raise DecodeError, message: "Unknown type: #{inspect(type)}"
  end

  def decode_uint8_list(num_items, rest, acc) when length(acc) == num_items,
    do: {Enum.reverse(acc), rest}

  def decode_uint8_list(num_items, <<int8, rest::binary>>, acc) do
    decode_uint8_list(num_items, rest, [int8 | acc])
  end

  def decode_uint32_list(num_items, rest, acc) when length(acc) == num_items,
    do: {Enum.reverse(acc), rest}

  def decode_uint32_list(num_items, <<int32::unsigned-native-32, rest::binary>>, acc) do
    decode_uint32_list(num_items, rest, [int32 | acc])
  end

  def decode_uint64_list(num_items, rest, acc) when length(acc) == num_items,
    do: {Enum.reverse(acc), rest}

  def decode_uint64_list(num_items, <<int64::unsigned-native-64, rest::binary>>, acc) do
    decode_uint64_list(num_items, rest, [int64 | acc])
  end

  def decode_float64_list(num_items, rest, acc) when length(acc) == num_items,
    do: {Enum.reverse(acc), rest}

  def decode_float64_list(num_items, <<float::96, rest::binary>>, acc) do
    decode_float64_list(num_items, rest, [float | acc])
  end

  def decode_map(
        num_pairs,
        <<@kStdString, _::binary>> = map,
        acc
      ) do
    {key, rest} = decode_value(map)
    {value, rest} = decode_value(rest)
    decode_map(num_pairs, rest, Map.put(acc, key, value))
  end

  def decode_map(num_pairs, rest, map) when map_size(map) == num_pairs, do: {map, rest}

  def decode_dynamic_list(num_items, rest, acc) when length(acc) == num_items,
    do: {Enum.reverse(acc), rest}

  def decode_dynamic_list(num_items, items, acc) do
    {value, rest} = decode_value(items)
    decode_dynamic_list(num_items, rest, [value | acc])
  end
end
