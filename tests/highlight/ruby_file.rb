# frozen_string_literal: true

module Demo
  class Greeter
    attr_reader :name

    def initialize(name: "world", **options)
      @name = name
      @options = { punctuation: "!", **options }
    end

    def label = "Hello, #{@name}#{@options[:punctuation]}"

    def call(...)
      puts(label, ...)
    end

    def classify(value)
      case value
      in { status: :ok, payload: [first, *rest] }
        { first:, remaining: rest.length }
      in Integer => count if count.positive?
        count.times.map { _1 * 2 }
      else
        nil
      end
    end
  end
end

message = <<~TEXT
  Ruby heredoc text
  spans multiple lines.
TEXT

Demo::Greeter.new(name: "Ada").call(message)

