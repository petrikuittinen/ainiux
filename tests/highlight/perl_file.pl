#!/usr/bin/env perl
use v5.36;
use strict;
use warnings;
use feature qw(signatures state say);

package Greeter;

sub new ($class, %args) {
    return bless { name => $args{name} // 'world' }, $class;
}

sub greet ($self, $punctuation = '!') {
    state $calls = 0;
    $calls++;
    my @roles = qw(admin editor);
    my %metadata = (calls => $calls, active => 1);
    my $message = <<~TEXT;
      Hello $self->{name}$punctuation
      Calls: $metadata{calls}
      TEXT
    return $message =~ s/^\s+//gr;
}

my $greeter = Greeter->new(name => 'Ada');
say $greeter->greet('?') if $greeter;

=pod
This POD block exercises multiline comments.
=cut

