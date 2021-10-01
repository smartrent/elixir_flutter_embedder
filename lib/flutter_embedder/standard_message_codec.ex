defmodule FlutterEmbedder.StandardMessageCodec do
  @kStdNull 0
  @kStdTrue 1
  @kStdFalse 2
  @kStdInt32 3
  @kStdInt64 4

  # we don't need largint?
  # https://github.com/flutter/flutter/blob/841beff5204ebff30b297cf6d4342b6b6db1bb39/packages/flutter/lib/src/services/message_codecs.dart#L359-L365
  # @kStdLargeInt 5

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
  @type value :: t()
  @type t() :: dart_string() | float64() | int32() | int64() | dart_map() | dynamic_list()

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
                  is_list(value) or
                  is_nil(value)

  defmodule Buffer do
    def new() do
      <<>>
    end

    def put_uint8(buffer, value) do
      buffer <> <<value::unsigned-8>>
    end

    def put_uint16(buffer, value) do
      buffer <> <<value::unsigned-native-16>>
    end

    def put_uint32(buffer, value) do
      buffer <> <<value::unsigned-native-32>>
    end

    def put_int32(buffer, value) do
      buffer <> <<value::signed-native-32>>
    end

    def put_int64(buffer, value) do
      buffer <> <<value::signed-native-64>>
    end

    def put_float64(buffer, value) do
      buffer = align_to(buffer, 8)
      buffer <> <<value::signed-native-float-64>>
    end

    def put_uint8_list(buffer, list) do
      buffer <> list
    end

    defp mod(x, y) when x > 0, do: rem(x, y)
    defp mod(x, y) when x < 0, do: rem(x, y) + y
    defp mod(0, _y), do: 0

    def align_to(buffer, alignment) do
      mod = mod(byte_size(buffer) + 1, alignment)
      if mod != 0 do
        pad = alignment - mod
        buffer <> <<0::size(pad)-unit(8)>>
      else
        buffer
      end
    end

    def write_size(buffer, size) when size < 254 do
      put_uint8(buffer, size)
    end

    def write_size(buffer, size) when size <= 0xFFFF do
      buffer
      |> put_uint8(254)
      |> put_uint16(size)
    end

    def write_size(buffer, size) when size <= 0xFFFFFFFF do
      buffer
      |> put_uint8(255)
      |> put_uint32(size)
    end
  end

  @spec encode_value(value()) :: binary()
  def encode_value(value) do
    write_value(value, Buffer.new())
  end

  # https://github.com/flutter/flutter/blob/841beff5204ebff30b297cf6d4342b6b6db1bb39/packages/flutter/lib/src/services/message_codecs.dart#L366
  def write_value(value, buffer) do
    cond do
      is_nil(value) ->
        Buffer.put_uint8(buffer, @kStdNull)

      value == true ->
        Buffer.put_uint8(buffer, @kStdTrue)

      value == false ->
        Buffer.put_uint8(buffer, @kStdFalse)

      is_float(value) ->
        buffer
        |> Buffer.put_uint8(@kStdFloat64)
        |> Buffer.put_float64(value)

      is_integer(value) and abs(value) <= 0x7FFFFFFF ->
        buffer
        |> Buffer.put_uint8(@kStdInt32)
        |> Buffer.put_int32(value)

      is_integer(value) and abs(value) <= 0x7FFFFFFFFFFFFFFF ->
        buffer
        |> Buffer.put_uint8(@kStdInt32)
        |> Buffer.put_int64(value)

      is_binary(value) ->
        buffer
        |> Buffer.put_uint8(@kStdString)
        |> Buffer.write_size(byte_size(value))
        |> Buffer.put_uint8_list(value)

      is_list(value) ->
        buffer
        |> Buffer.put_uint8(@kStdList)
        |> Buffer.write_size(length(value))
        |> (fn buffer ->
              Enum.reduce(value, buffer, &write_value/2)
            end).()

      is_map(value) ->
        buffer
        |> Buffer.put_uint8(@kStdMap)
        |> Buffer.write_size(map_size(value))
        |> (fn buffer ->
              Enum.reduce(value, buffer, fn {key, value}, acc ->
                acc = write_value(to_string(key), acc)
                write_value(value, acc)
              end)
            end).()

      true ->
        raise ArgumentError, value: value
    end
  end

  def encode_term(nil), do: <<@kStdNull>>
  def encode_term(true), do: <<@kStdTrue>>
  def encode_term(false), do: <<@kStdFalse>>

  def encode_term(int32) when is_integer(int32) and abs(int32) <= 0x7FFFFFFF,
    do: <<@kStdInt32, int32::signed-native-32>>

  def encode_term(int64) when is_integer(int64) and abs(int64) <= 0x7FFFFFFFFFFFFFFF,
    do: <<@kStdInt64, int64::signed-native-64>>

  def encode_term(float64) when is_float(float64),
    do: <<@kStdFloat64, 0::6*8, float64::signed-native-float-64>>

  def encode_term(string) when is_binary(string) and byte_size(string) < 254 do
    <<@kStdString, byte_size(string)::8, string::binary>>
  end

  def encode_term(string) when is_binary(string) and byte_size(string) < 0xFFFF do
    <<@kStdString, 254, byte_size(string)::native-16, string::binary>>
  end

  # TODO encode @kStdUInt8Array, @kStdInt32Array, @kStdInt64Array, @kStdFloat64Array
  def encode_term(value) when is_list(value) do
    acc = <<@kStdList, length(value)::8>>

    Enum.reduce(value, acc, fn
      value, acc when is_valid_dart_value(value) ->
        acc <> encode_term(value)

      _invalid, _acc ->
        raise ArgumentError
    end)
  end

  # i don't think Dart actually allows for maps as return values via PlatformChannel
  def encode_term(%{} = map) do
    acc = <<@kStdMap, map_size(map)::8>>

    Enum.reduce(map, acc, fn
      # Dart only allows string keys
      {key, value}, acc when is_binary(key) and is_valid_dart_value(value) ->
        acc <> encode_term(key) <> encode_term(value)

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
