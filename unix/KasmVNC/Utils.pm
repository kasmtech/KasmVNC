package KasmVNC::Utils;

use strict;
use warnings;
use v5.10;
use Data::Dumper;
use Switch;

use Exporter;

@KasmVNC::Utils::ISA = qw(Exporter);

our @EXPORT = ('listify', 'flatten', 'isBlank', 'isPresent', 'deriveBoolean',
  'printStackTrace', 'interpolateEnvVars');

sub listify {
    # Implementation based on Hyper::Functions
    if (scalar @_ > 1) {
        return [ @_ ];
    } elsif (defined $_[0]) {
        my $ref_type = ref $_[0];
        return ($ref_type && $ref_type eq 'ARRAY') ? $_[0] : [ $_[0] ];
    } else {
        return [];
    }
}

sub flatten {
  map { ref $_ ? flatten(@{$_}) : $_ } @_;
}

sub isBlank {
  !isPresent(shift);
}

sub isPresent {
  my $value = shift;
  if (ref($value) eq "HASH") {
    return scalar(keys %$value) > 0;
  }

  defined($value);
}

sub deriveBoolean {
  my $value = shift;

  return $value if containsWideSymbols($value);

  switch($value) {
    case 'true' {
      return 1;
    }
    case 'false' {
      return 0;
    }
    else {
      return $value;
    }
  }
}

sub printStackTrace {
  my $trace = Devel::StackTrace->new;
  print { *STDERR } $trace->as_string;
}

sub containsWideSymbols {
	my $string = shift;

  return 1 unless defined($string);

	$string =~ /[^\x00-\xFF]/;
}

sub interpolateEnvVars {
  my $value = shift;

  return $value unless defined($value);

  while ($value =~ /\$\{(\w+)\}/) {
    my $envValue = $ENV{$1};
    $value =~ s/\Q$&\E/$envValue/;
  }

  $value;
}

1;
