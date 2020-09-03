defmodule FlutterEmbedder.StandardCallCodec do
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

  def decode_value(<<@kStdString, length::8, string::binary-size(length), rest::binary>>)
      when length < 254,
      do: {string, rest}

  def decode_value(<<@kStdString, base, add, mult, string::binary>>) do
    strlen = (base + (256 - base)) * mult + add
    <<string::binary-size(strlen), rest::binary>> = string
    {string, rest}
  end

  def decode_value(<<@kStdFloat64, float::96, rest::binary>>), do: {float, rest}
  def decode_value(<<@kStdInt64, int64::signed-native-64, rest::binary>>), do: {int64, rest}
  def decode_value(<<@kStdInt32, int32::signed-native-32, rest::binary>>), do: {int32, rest}
  def decode_value(<<@kStdFalse, rest::binary>>), do: {false, rest}
  def decode_value(<<@kStdTrue, rest::binary>>), do: {true, rest}
  def decode_value(<<@kStdNull, rest::binary>>), do: {nil, rest}

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
