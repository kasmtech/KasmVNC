use DateTime::TimeZone;

my $timezone = $ARGV[0];

if (DateTime::TimeZone->is_valid_name($timezone)) {
    print "Valid timezone\n";
} else {
    print "Invalid timezone\n";
}
