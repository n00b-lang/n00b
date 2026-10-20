# hi
class Foo
  @@count = 0
  def initialize(n)
    @name = n
    $global = 1
  end
  def to_s
    "name=#{@name}"
  end
end
