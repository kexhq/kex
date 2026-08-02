# A known-good reference for Kex's Date / Time / DateTime stdlib.
#
# Every method here mirrors one in src/stdlib/time.kex, but is implemented
# with Ruby's built-in calendar (`Date`, `Time`) rather than by reimplementing
# the arithmetic. That is the whole point: when the two disagree, Ruby is
# right and Kex is wrong.
#
# Ruby's `Date` is asked for the PROLEPTIC Gregorian calendar throughout
# (`Date::GREGORIAN`), because that is the calendar Kex models — no Julian
# switchover in 1582, and astronomical year numbering where year 0 is 1 BC.
#
# Used by gen_spec.rb, which turns these answers into a Kex spec file.

require "date"

module CalendarRef
  # Ruby counts days from the Julian Day Number; Kex counts them from
  # 1970-01-01. This is the offset between the two.
  UNIX_EPOCH_JD = 2440588

  MONTH_NAMES = %w[January February March April May June July
                   August September October November December].freeze

  WEEKDAY_NAMES = %w[Monday Tuesday Wednesday Thursday Friday
                     Saturday Sunday].freeze

  module_function

  # ── Bridging to Ruby's Date ─────────────────────────────────────────────

  # A proleptic-Gregorian Ruby Date. Raises Date::Error on a field that is
  # not a real calendar date, which is what `valid_date?` below reports.
  def civil(year, month, day)
    Date.new(year, month, day, Date::GREGORIAN)
  end

  def valid_date?(year, month, day)
    civil(year, month, day)
    true
  rescue Date::Error
    false
  end

  # ── Time.leapYear? / Time.daysInMonth ───────────────────────────────────

  def leap_year?(year)
    Date.gregorian_leap?(year)
  end

  # Kex argument order: year first, matching Date.of(year, month, day).
  # A month outside 1..12 has no answer; the caller decides what to do.
  def days_in_month(year, month)
    raise ArgumentError, "month out of range: #{month}" unless (1..12).cover?(month)

    # -1 is Ruby's "last day of the month".
    Date.new(year, month, -1, Date::GREGORIAN).day
  end

  # ── Time.daysFromCivil / Time.civilFromDays ─────────────────────────────

  def days_from_civil(year, month, day)
    civil(year, month, day).jd - UNIX_EPOCH_JD
  end

  # Returns [year, month, day].
  def civil_from_days(epoch_day)
    date = Date.jd(epoch_day + UNIX_EPOCH_JD).gregorian
    [date.year, date.month, date.day]
  end

  # ── Weekdays ────────────────────────────────────────────────────────────

  # ISO numbering: Monday is 1, Sunday is 7. Ruby's `cwday` is already ISO.
  def weekday_number_from_epoch_day(epoch_day)
    Date.jd(epoch_day + UNIX_EPOCH_JD).cwday
  end

  def weekday_name_from_epoch_day(epoch_day)
    WEEKDAY_NAMES[weekday_number_from_epoch_day(epoch_day) - 1]
  end

  def weekend_from_epoch_day?(epoch_day)
    weekday_number_from_epoch_day(epoch_day) > 5
  end

  # ── Date methods ────────────────────────────────────────────────────────

  def day_of_year(year, month, day)
    civil(year, month, day).yday
  end

  def add_days(year, month, day, count)
    date = civil(year, month, day) + count
    [date.year, date.month, date.day]
  end

  # Calendar-aware: the day is clamped into the target month, so one month
  # after January 31st is the last day of February. Ruby's `>>` clamps the
  # same way.
  def add_months(year, month, day, count)
    date = civil(year, month, day) >> count
    [date.year, date.month, date.day]
  end

  def add_years(year, month, day, count)
    add_months(year, month, day, count * 12)
  end

  def days_until(from, to)
    (civil(*to) - civil(*from)).to_i
  end

  # ── Formatting ──────────────────────────────────────────────────────────

  # ISO 8601 keeps four digits where the year fits; wider or negative years
  # render as-is, which is what Kex's padYear does.
  def format_year(year)
    return year.to_s if year.negative? || year > 9999

    format("%04d", year)
  end

  def format_date(year, month, day)
    "#{format_year(year)}-#{format('%02d', month)}-#{format('%02d', day)}"
  end

  def format_time(hour, minute, second)
    format("%02d:%02d:%02d", hour, minute, second)
  end

  # ±HH:MM, the shape ISO 8601 gives an offset. UTC renders as "Z".
  def format_offset(offset_seconds)
    return "Z" if offset_seconds.zero?

    sign = offset_seconds.negative? ? "-" : "+"
    magnitude = offset_seconds.abs
    "#{sign}#{format('%02d:%02d', magnitude / 3600, (magnitude % 3600) / 60)}"
  end

  def format_date_time(date, time, offset_seconds)
    format_date(*date) + "T" + format_time(*time) + format_offset(offset_seconds)
  end

  # ── Time of day ─────────────────────────────────────────────────────────

  def valid_time?(hour, minute, second, nanosecond = 0)
    (0..23).cover?(hour) && (0..59).cover?(minute) &&
      (0..59).cover?(second) && (0..999_999_999).cover?(nanosecond)
  end

  def seconds_since_midnight(hour, minute, second)
    hour * 3600 + minute * 60 + second
  end

  # Wraps, so 86400 is midnight again. Returns [hour, minute, second].
  def from_seconds_since_midnight(count)
    total = count % 86_400
    [total / 3600, (total % 3600) / 60, total % 60]
  end

  # ── Instants ────────────────────────────────────────────────────────────

  # Seconds since the Unix epoch for a civil datetime rendered at an offset.
  def epoch_seconds(date, time, offset_seconds)
    days_from_civil(*date) * 86_400 + seconds_since_midnight(*time) - offset_seconds
  end

  # The inverse: the civil rendering of an instant at a given offset.
  # Returns [[year, month, day], [hour, minute, second]].
  def from_epoch_seconds(count, offset_seconds = 0)
    local = count + offset_seconds
    # Floor division: instants before the epoch still land on the day that
    # contains them.
    day = local.fdiv(86_400).floor
    [civil_from_days(day), from_seconds_since_midnight(local - day * 86_400)]
  end
end
